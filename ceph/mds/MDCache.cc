
#include "MDCache.h"
#include "MDStore.h"
#include "CInode.h"
#include "CDir.h"
#include "MDS.h"
#include "MDCluster.h"
#include "MDLog.h"

#include "include/filepath.h"

#include "include/Message.h"
#include "include/Messenger.h"

#include "events/EInodeUpdate.h"

#include "messages/MDiscover.h"
#include "messages/MInodeGetReplica.h"
#include "messages/MInodeGetReplicaAck.h"

#include "messages/MExportDirPrep.h"
#include "messages/MExportDirPrepAck.h"
#include "messages/MExportDir.h"
#include "messages/MExportDirAck.h"
#include "messages/MExportDirNotify.h"

#include "messages/MHashDir.h"
#include "messages/MUnhashDir.h"
#include "messages/MUnhashDirAck.h"

#include "messages/MInodeUpdate.h"
#include "messages/MDirUpdate.h"

#include "messages/MInodeExpire.h"

#include "messages/MInodeSyncStart.h"
#include "messages/MInodeSyncAck.h"
#include "messages/MInodeSyncRelease.h"
#include "messages/MInodeSyncRecall.h"

#include "messages/MInodeLockStart.h"
#include "messages/MInodeLockAck.h"
#include "messages/MInodeLockRelease.h"

#include "InoAllocator.h"

#include <assert.h>
#include <errno.h>
#include <iostream>
#include <string>
#include <map>
using namespace std;

#include "include/config.h"
#undef dout
#define  dout(l)    if (l<=g_conf.debug) cout << "mds" << mds->get_nodeid() << ".cache "



MDCache::MDCache(MDS *m)
{
  mds = m;
  root = NULL;
  opening_root = false;
  lru = new LRU();
  lru->lru_set_max(g_conf.mdcache_size);
  lru->lru_set_midpoint(g_conf.mdcache_mid);

  inoalloc = new InoAllocator(mds);
}

MDCache::~MDCache() 
{
  if (lru) { delete lru; lru = NULL; }
  if (inoalloc) { delete inoalloc; inoalloc = NULL; }
}


// 

bool MDCache::shutdown()
{
  if (lru->lru_get_size() > 0) {
	dout(7) << "WARNING: mdcache shutodwn with non-empty cache" << endl;
	show_cache();
	show_imports();
  }
}


// MDCache

CInode *MDCache::create_inode()
{
  CInode *in = new CInode;

  // zero
  memset(&in->inode, 0, sizeof(inode_t));
  
  // assign ino
  in->inode.ino = inoalloc->get_ino();

  add_inode(in);  // add
  return in;
}

void MDCache::destroy_inode(CInode *in)
{
  inoalloc->reclaim_ino(in->ino());
  remove_inode(in);
}


bool MDCache::add_inode(CInode *in) 
{
  // add to lru, inode map
  assert(inode_map.size() == lru->lru_get_size());
  lru->lru_insert_mid(in);
  inode_map[ in->ino() ] = in;
  assert(inode_map.size() == lru->lru_get_size());
}

bool MDCache::remove_inode(CInode *o) 
{
  // detach from parents
  if (o->nparents == 1) {
	CDentry *dn = o->parent;
	dn->dir->remove_child(dn);
	o->remove_parent(dn);
	delete dn;
  } 
  else if (o->nparents > 1) {
	assert(o->nparents <= 1);
  } else {
	assert(o->nparents == 0);  // root.
	assert(o->parent == NULL);
  }

  // remove from map
  inode_map.erase(o->ino());

  // remove from lru
  lru->lru_remove(o);

  return true;
}

bool MDCache::trim(__int32_t max) {
  if (max < 0) {
	max = lru->lru_get_max();
	if (!max) return false;
  }

  while (lru->lru_get_size() > max) {
	CInode *in = (CInode*)lru->lru_expire();
	if (!in) return false;

	// notify authority?
	int auth = in->authority(mds->get_cluster());
	if (auth != mds->get_nodeid()) {
	  dout(7) << "sending inode_expire to mds" << auth << " on " << *in << endl;
	  mds->messenger->send_message(new MInodeExpire(in->ino(), mds->get_nodeid()),
								   MSG_ADDR_MDS(auth), MDS_PORT_CACHE,
								   MDS_PORT_CACHE);
	}	

	CInode *idir = NULL;
	if (in->parent)
	  idir = in->parent->dir->inode;

	// remove it
	dout(12) << "trim deleting " << *in << " " << in << endl;
	remove_inode(in);
	delete in;

	if (idir) {
	  // dir incomplete!
	  idir->dir->state_clear(CDIR_STATE_COMPLETE);

	  // reexport?
	  if (imports.count(idir) &&                // import
		  idir->dir->get_size() == 0 &&         // no children
		  !idir->is_root() &&                   // not root
		  !(idir->dir->is_freezing() || idir->dir->is_frozen())  // FIXME: can_auth_pin?
		  ) {
		int dest = idir->authority(mds->get_cluster());

		// comment this out ot wreak havoc?
		if (mds->is_shutting_down()) dest = 0;  // this is more efficient.

		if (dest != mds->get_nodeid()) {
		  // it's an empty import!
		  dout(7) << "trimmed parent dir is a (now empty) import; rexporting to " << dest << endl;
		  export_dir( idir, dest );
		}
	  }
	} else {
	  dout(7) << " that was root!" << endl;
	  root = NULL;
	}
  }
  
  return true;
}


void MDCache::shutdown_start()
{
  dout(1) << "unsync, unlock everything" << endl;

  // walk cache
  bool didsomething = false;
  for (hash_map<inodeno_t, CInode*>::iterator it = inode_map.begin();
	   it != inode_map.end();
	   it++) {
	CInode *in = it->second;
	if (in->is_auth()) {
	  if (in->is_syncbyme()) sync_release(in);
	  if (in->is_lockbyme()) inode_lock_release(in);
	}
  }

  // make sure sticky sync is off
  g_conf.mdcache_sticky_sync_normal = false;

}

bool MDCache::shutdown_pass()
{
  static bool did_inode_updates = false;

  dout(7) << "shutdown_pass" << endl;
  //assert(mds->is_shutting_down());
  if (mds->is_shut_down()) {
	cout << " already shut down" << endl;
	show_cache();
	show_imports();
	return true;
  }

  // make a pass on the cache
  
  if (mds->mdlog->get_num_events()) {
	dout(7) << "waiting for log to flush" << endl;
	return false;
  } 

  dout(7) << "log is empty; flushing cache" << endl;
  trim(0);
  
  // walk cache
  dout(7) << "walking remaining cache for items cached_by shut down nodes" << endl;
  bool didsomething = false;
  for (hash_map<inodeno_t, CInode*>::iterator it = inode_map.begin();
	   it != inode_map.end();
	   it++) {
	CInode *in = it->second;
	if (in->is_auth()) {
	  // cached_by
	  // unpin inodes on shut down nodes.
	  // NOTE: this happens when they expire during an export; expires reference inodes, and can thus
	  // be missed.
	  if (mds->get_nodeid() == 0 &&
		  in->is_cached_by_anyone()) {
		for (set<int>::iterator by = in->cached_by.begin();
			 by != in->cached_by.end();
			 ) {
		  int who = *by;
		  by++;
		  if (mds->is_shut_down(who)) {
			in->cached_by_remove(who);
			didsomething = true;
		  }
		}
	  }
	}
  }
  if (didsomething)
	trim(0);
  
  dout(7) << "cache size now " << lru->lru_get_size() << endl;

  // send inode_expire's on all potentially cache pinned items
  if (false &&
	  !did_inode_updates) {
	did_inode_updates = true;

	for (hash_map<inodeno_t, CInode*>::iterator it = inode_map.begin();
		 it != inode_map.end();
		 it++) {
	  if (it->second->ref_set.count(CINODE_PIN_CACHED)) 
		send_inode_updates(it->second);  // send an update to discover who dropped the ball
	}
  }

  // send imports to 0!
  if (mds->get_nodeid() != 0) {
	for (set<CInode*>::iterator it = imports.begin();
		 it != imports.end();
		 ) {
	  CInode *im = *it;
	  it++;
	  if (im->is_root()) continue;
	  if (im->dir->is_frozen() || im->dir->is_freezing()) continue;
	  
	  dout(7) << "sending " << *im << " back to mds0" << endl;
	  export_dir(im,0);
	}
  } else {
	// shut down root?
	if (lru->lru_get_size() == 1) {
	  // all i have left is root.. wtf?
	  dout(7) << "wahoo, all i have left is root!" << endl;
	  
	  // un-import.
	  imports.erase(root);
	  root->dir->state_clear(CDIR_STATE_IMPORT);
	  root->put(CINODE_PIN_IMPORT);

	  if (root->is_pinned_by(CINODE_PIN_DIRTY))   // no root storage yet.
		root->put(CINODE_PIN_DIRTY);
	  
	  if (root->ref != 0) {
		dout(1) << "ugh, bad shutdown!" << endl;
		imports.insert(root);
		root->dir->state_set(CDIR_STATE_IMPORT);
		root->get(CINODE_PIN_IMPORT);
	  } else 
		trim(0);	  // trim it
	  
	  show_cache();
	  show_imports();
	}
  }
	
  // and?
  assert(inode_map.size() == lru->lru_get_size());
  if (lru->lru_get_size() == 0) {
	if (mds->get_nodeid() != 0) {
	  dout(7) << "done, sending shutdown_finish" << endl;
	  mds->messenger->send_message(new Message(MSG_MDS_SHUTDOWNFINISH),
								   MSG_ADDR_MDS(0), MDS_PORT_MAIN, MDS_PORT_MAIN);
	} else {
	  mds->handle_shutdown_finish(NULL);
	}
	return true;
  } else {
	dout(7) << "there's still stuff in the cache: " << lru->lru_get_size() << endl;
  }
  return false;
}




int MDCache::link_inode( CInode *parent, string& dname, CInode *in ) 
{
  if (!parent->dir) {
	return -ENOTDIR;  // not a dir
  }

  assert(parent->dir->lookup(dname) == 0);

  // create dentry
  CDentry* dn = new CDentry(dname, in);
  in->add_parent(dn);

  // add to dir
  parent->dir->add_child(dn);

  // set dir version
  in->parent_dir_version = parent->dir->get_version();

  return 0;
}




int MDCache::open_root(Context *c)
{
  int whoami = mds->get_nodeid();

  // open root inode
  if (whoami == 0) { 
	// i am root
	CInode *root = new CInode();
	root->inode.ino = 1;
	root->inode.isdir = true;

	// make it up (FIXME)
	root->inode.mode = 0755;
	root->inode.size = 0;
	root->inode.touched = 0;

	root->dir = new CDir(root, whoami);
	assert(root->dir->is_auth());
	root->dir_auth = 0;  // me!
	root->dir->dir_rep = CDIR_REP_NONE;

	set_root( root );

	// root is technically an import (from a vacuum)
	imports.insert( root );
	root->dir->state_set(CDIR_STATE_IMPORT);
	root->get(CINODE_PIN_IMPORT);

	if (c) {
	  c->finish(0);
	  delete c;
	}
  } else {
	// request inode from root mds
	if (c) 
	  waiting_for_root.push_back(c);
	
	if (!opening_root) {
	  dout(7) << "discovering root" << endl;
	  opening_root = true;

	  MDiscover *req = new MDiscover(whoami,
									 string(""),
									 NULL);
	  mds->messenger->send_message(req,
								   MSG_ADDR_MDS(0), MDS_PORT_CACHE,
								   MDS_PORT_CACHE);
	} else
	  dout(7) << "waiting for root" << endl;
	
  }
}


CInode *MDCache::get_containing_import(CInode *in)
{
  CInode *imp = in;  // might be *in

  // find the underlying import!
  while (imp && 
		 imports.count(imp) == 0) {
	imp = imp->get_parent_inode();
  }

  assert(imp);
  return imp;
}

CInode *MDCache::get_containing_export(CInode *in)
{
  CInode *ex = in;  // might be *in

  // find the underlying import!
  while (ex &&                        // white not at root,
		 exports.count(ex) == 0) {    // we didn't find an export,
	ex = ex->get_parent_inode();
  }

  return ex;
}









// ========= messaging ==============


int MDCache::proc_message(Message *m)
{
  switch (m->get_type()) {
  case MSG_MDS_DISCOVER:
	handle_discover((MDiscover*)m);
	break;


  case MSG_MDS_INODEUPDATE:
	handle_inode_update((MInodeUpdate*)m);
	break;

  case MSG_MDS_DIRUPDATE:
	handle_dir_update((MDirUpdate*)m);
	break;

  case MSG_MDS_INODEEXPIRE:
	handle_inode_expire((MInodeExpire*)m);
	break;


	// sync
  case MSG_MDS_INODESYNCSTART:
	handle_inode_sync_start((MInodeSyncStart*)m);
	break;
  case MSG_MDS_INODESYNCACK:
	handle_inode_sync_ack((MInodeSyncAck*)m);
	break;
  case MSG_MDS_INODESYNCRELEASE:
	handle_inode_sync_release((MInodeSyncRelease*)m);
	break;
  case MSG_MDS_INODESYNCRECALL:
	handle_inode_sync_recall((MInodeSyncRecall*)m);
	break;
	
	// lock
  case MSG_MDS_INODELOCKSTART:
	handle_inode_lock_start((MInodeLockStart*)m);
	break;
  case MSG_MDS_INODELOCKACK:
	handle_inode_lock_ack((MInodeLockAck*)m);
	break;
  case MSG_MDS_INODELOCKRELEASE:
	handle_inode_lock_release((MInodeLockRelease*)m);
	break;
	


	// import
  case MSG_MDS_EXPORTDIRPREP:
	handle_export_dir_prep((MExportDirPrep*)m);
	break;

  case MSG_MDS_EXPORTDIR:
	handle_export_dir((MExportDir*)m);
	break;

	// export 
  case MSG_MDS_EXPORTDIRPREPACK:
	handle_export_dir_prep_ack((MExportDirPrepAck*)m);
	break;
	
  case MSG_MDS_EXPORTDIRACK:
	handle_export_dir_ack((MExportDirAck*)m);
	break;
	
	// export 3rd party (inode authority)
  case MSG_MDS_EXPORTDIRNOTIFY:
	handle_export_dir_notify((MExportDirNotify*)m);
	break;

	
  default:
	dout(7) << "cache unknown message " << m->get_type() << endl;
	assert(0);
	break;
  }

  return 0;
}


/* path_traverse
 *
 * return values:
 *   <0 : traverse error (ENOTDIR, ENOENT)
 *    0 : success
 *   >0 : delayed or forwarded
 */
int MDCache::path_traverse(string& pathname, 
						   vector<CInode*>& trace, 
						   Message *req,
						   int onfail)
{
  int whoami = mds->get_nodeid();
  
  CInode *cur = get_root();
  if (cur == NULL) {
	dout(7) << "mds" << whoami << " i don't have root" << endl;
	if (req) 
	  open_root(new C_MDS_RetryMessage(mds, req));
	return 1;
  }

  // break path into bits.
  trace.clear();
  trace.push_back(cur);

  // get read access
  //if (read_wait(cur, req))
  //	return 1;   // wait

  string have_clean;

  filepath path = pathname;

  for (int depth = 0; depth < path.depth(); depth++) {
	string dname = path[depth];
	//dout(7) << " path seg " << dname << endl;

	// lookup dentry
	if (cur->is_dir()) {
	  if (!cur->dir)
		cur->dir = new CDir( cur, whoami );
	  
	  // frozen?
	  if (cur->dir->is_frozen_tree_root() ||
		  cur->dir->is_frozen_dir()) {
		// doh!
		// FIXME: traverse is allowed?
		dout(7) << " dir " << *cur << " is frozen, waiting" << endl;
		cur->dir->add_waiter(CDIR_WAIT_UNFREEZE,
							 new C_MDS_RetryMessage(mds, req));
		return 1;
	  }

	  // must read hard data to traverse
	  if (!read_hard_try(cur, req))
		return 1;

	  // check permissions?


	  // dentry:	  
	  CDentry *dn = cur->dir->lookup(dname);
	  if (dn && dn->inode) {
		// have it, keep going.
		cur = dn->inode;
		have_clean += "/";
		have_clean += dname;
	  } else {
		// don't have it.
		int dauth = cur->dir->dentry_authority( dname, mds->get_cluster() );

		if (dauth == whoami) {
		  // mine.
		  if (cur->dir->is_complete()) {
			// file not found
			return -ENOENT;
		  } else {
			if (onfail == MDS_TRAVERSE_DISCOVER) 
			  return -1;

			// directory isn't complete; reload
			dout(7) << "mds" << whoami << " incomplete dir contents for " << *cur << ", fetching" << endl;
			lru->lru_touch(cur);  // touch readdiree
			mds->mdstore->fetch_dir(cur, new C_MDS_RetryMessage(mds, req));

			mds->logger->inc("cmiss");
			mds->logger->inc("rdir");
			return 1;		   
		  }
		} else {
		  // not mine.

		  if (onfail == MDS_TRAVERSE_DISCOVER) {
			// discover
			dout(7) << " discover on " << *cur << " for " << dname << "..., to mds" << dauth << endl;

			// assemble+send request
			vector<string> *want = new vector<string>;
			for (int i=depth; i<path.depth(); i++)
			  want->push_back(path[i]);

			lru->lru_touch(cur);  // touch discoveree

			mds->messenger->send_message(new MDiscover(whoami, have_clean, want),
									MSG_ADDR_MDS(dauth), MDS_PORT_CACHE,
									MDS_PORT_CACHE);
			
			// delay processing of current request
			cur->dir->add_waiter(CDIR_WAIT_DENTRY, dname, new C_MDS_RetryMessage(mds, req));

			mds->logger->inc("dis");
			mds->logger->inc("cmiss");
			return 1;
		  } 
		  if (onfail == MDS_TRAVERSE_FORWARD) {
			// forward
			dout(7) << " not authoritative for " << dname << ", fwd to mds" << dauth << endl;
			mds->messenger->send_message(req,
										 MSG_ADDR_MDS(dauth), req->get_dest_port(),
										 req->get_dest_port());
			//show_imports();

			mds->logger->inc("cfw");
			return 1;
		  }	
		  if (onfail == MDS_TRAVERSE_FAIL) {
			return -1;  // -ENOENT, but only because i'm not the authority
		  }
		}
	  }
	} else {
	  dout(7) << *cur << " not a dir " << cur->inode.isdir << endl;
	  return -ENOTDIR;
	}
	
	trace.push_back(cur);
	//read_wait(cur, req);  // wait for read access
  }

  // success.
  return 0;
}






// REPLICAS


int MDCache::handle_discover(MDiscover *dis) 
{
  int whoami = mds->get_nodeid();

  if (dis->get_asker() == whoami) {
	// this is a result
	vector<MDiscoverRec_t> trace = dis->get_trace();
	
	if (dis->just_root()) {
	  dout(7) << "handle_discover got root" << endl;
	  
	  CInode *root = new CInode();
	  root->inode = trace[0].inode;
	  root->cached_by = trace[0].cached_by;
	  root->cached_by.insert(whoami);   // obviously i have it too
	  root->dir_auth = trace[0].dir_auth;
	  root->dir = new CDir(root, whoami); // not auth
	  assert(!root->dir->is_auth());
	  root->dir->dir_rep = trace[0].dir_rep;
	  root->dir->dir_rep_by = trace[0].dir_rep_by;
	  root->auth = false;
	  
	  if (trace[0].is_syncbyauth) root->dist_state |= CINODE_DIST_SYNCBYAUTH;
	  if (trace[0].is_softasync) root->dist_state |= CINODE_DIST_SOFTASYNC;
	  if (trace[0].is_lockbyauth) root->dist_state |= CINODE_DIST_LOCKBYAUTH;

	  set_root( root );

	  opening_root = false;

	  // done
	  delete dis;

	  // finish off.
	  list<Context*> finished;
	  finished.splice(finished.end(), waiting_for_root);

	  list<Context*>::iterator it;
	  for (it = finished.begin(); it != finished.end(); it++) {
		Context *c = *it;
		c->finish(0);
		delete c;
	  }

	  return 0;
	}
	
	// traverse to start point
	vector<CInode*> trav;

	dout(7) << "handle_discover got result" << endl;
	  
	int r = path_traverse(dis->get_basepath(), trav, NULL, MDS_TRAVERSE_FAIL);   // FIXME BUG??
	if (r < 0) {
	  dout(1) << "handle_discover result, but not in cache any more.  dropping." << endl;
	  delete dis;
	  return 0;
	}
	
	CInode *cur = trav[trav.size()-1];
	CInode *start = cur;

	vector<string> *wanted = dis->get_want();

	list<Context*> finished;

	// add duplicated dentry+inodes
	for (int i=0; i<trace.size(); i++) {

	  if (!cur->dir) cur->dir = new CDir(cur, whoami);

	  CInode *in;
	  CDentry *dn = cur->dir->lookup( (*wanted)[i] );
	  
	  int dentry_auth = cur->dir->dentry_authority( dn->name, mds->get_cluster() );

	  if (dn) {
		// already had it?  (parallel discovers?)
		dout(7) << "huh, already had " << (*wanted)[i] << endl;
		in = dn->inode;
	  } else {
		if (dentry_auth == whoami) {
		  // uh oh, discover has something that's ours, and we don't have.  readdir and delay!
		  dout(3) << "huh, dentry has item " << *cur << " dentry " << dn->name << ", which is ours, but we don't have.  fetching dir!" << endl;
		  mds->mdstore->fetch_dir(cur,
								  new C_MDS_RetryMessage(mds, dis));
		  return 0;
		}


		in = new CInode();

		// assim discover info
		in->inode = trace[i].inode;
		in->cached_by = trace[i].cached_by;
		in->cached_by.insert(whoami);    // obviously i have it too
		in->dir_auth = trace[i].dir_auth;

		in->auth = false;

		if (in->is_dir()) {
		  in->dir = new CDir(in, whoami);   // can't be ours (an import) or it'd be in our cache.
		  assert(!in->dir->is_auth());
		  in->dir->dir_rep = trace[i].dir_rep;
		  in->dir->dir_rep_by = trace[i].dir_rep_by;
		  assert(!in->dir->is_auth());
		}

		if (trace[i].is_syncbyauth) in->dist_state |= CINODE_DIST_SYNCBYAUTH;
		if (trace[i].is_softasync) in->dist_state |= CINODE_DIST_SOFTASYNC;
		if (trace[i].is_lockbyauth) in->dist_state |= CINODE_DIST_LOCKBYAUTH;
		
		// link in
		add_inode( in );
		link_inode( cur, (*wanted)[i], in );

		dout(7) << " discover assimilating " << *in << endl;
	  }
	  
	  cur->dir->take_waiting(CDIR_WAIT_DENTRY,
							 (*wanted)[i],
							 finished);
	  
	  cur = in;
	}

	// done
	delete dis;

	// finish off waiting items
	dout(7) << " i have " << finished.size() << " contexts to finish" << endl;
	list<Context*>::iterator it;
	for (it = finished.begin(); it != finished.end(); it++) {
	  Context *c = *it;
	  c->finish(0);
	  delete c;				
	}	

  } else {
	
	dout(7) << "handle_discover from mds" << dis->get_asker() << " current_need() " << dis->current_need() << endl;
	
	// this is a request
	if (!root) {
	  //open_root(new C_MDS_RetryMessage(mds, dis));
	  dout(7) << "don't have root, just sending to mds0" << endl;
	  mds->messenger->send_message(dis,
								   MSG_ADDR_MDS(0), MDS_PORT_CACHE,
								   MDS_PORT_CACHE);
	  return 0;
	}

	// get to starting point
	vector<CInode*> trav;
	string current_base = dis->current_base();
	int r = path_traverse(current_base, trav, dis, MDS_TRAVERSE_FORWARD);
	if (r > 0) return 0;  // forwarded, i hope!
	
	CInode *cur = trav[trav.size()-1];
	
	// just root?
	if (dis->just_root()) {
	  CInode *root = get_root();
	  dis->add_bit( root, 0 );

	  root->cached_by_add(dis->get_asker());
	}

	// add bits
	bool have_added = false;
	while (!dis->done()) {
	  if (!cur->is_dir()) {
		dout(7) << "woah, discover on non dir " << dis->current_need() << endl;
		assert(cur->is_dir());
	  }

	  if (!cur->dir) cur->dir = new CDir(cur, whoami);
	  
	  string next_dentry = dis->next_dentry();
	  int dentry_auth = cur->dir->dentry_authority(next_dentry, mds->get_cluster());
	  
	  if (dentry_auth != whoami) {
		if (have_added) // fwd (partial) results back to asker
		  mds->messenger->send_message(dis,	
									   MSG_ADDR_MDS(dis->get_asker()), MDS_PORT_CACHE,
									   MDS_PORT_CACHE);
		else  		// fwd to authority
		  mds->messenger->send_message(dis,
									   MSG_ADDR_MDS(dentry_auth), MDS_PORT_CACHE,
									   MDS_PORT_CACHE);
		return 0;
	  }

	  // ok, i'm the authority for this dentry!

	  // if frozen: i can't add replica because i'm no longer auth
	  if (cur->dir->is_frozen()) {
		dout(7) << " dir " << *cur << " is frozen, waiting" << endl;
		cur->dir->add_waiter(CDIR_WAIT_UNFREEZE,
							 new C_MDS_RetryMessage(mds, dis));
		return 0;
	  }

	  // lookup next bit
	  CDentry *dn = cur->dir->lookup(dis->next_dentry());
	  if (dn) {	
		// yay!  
		CInode *next = dn->inode;

		dout(7) << "discover adding bit " << *next << " for mds" << dis->get_asker() << endl;
		
		// add it
		dis->add_bit( next, whoami );
		have_added = true;
		
		// remember who is caching this!
		next->cached_by_add( dis->get_asker() );
		
		cur = next; // continue!
	  } else {
		// don't have dentry.

		// are we auth for this dir?
		if (cur->dir->is_complete()) {
		  // file not found.
		  assert(!cur->dir->is_complete());
		} else {
		  // readdir
		  dout(7) << "mds" << whoami << " incomplete dir contents for " << *cur << ", fetching" << endl;
		  mds->mdstore->fetch_dir(cur, new C_MDS_RetryMessage(mds, dis));
		  return 0;
		}
	  }
	}
	
	// success, send result
	dout(7) << "mds" << whoami << " finished discovery, sending back to " << dis->get_asker() << endl;
	mds->messenger->send_message(dis,
								 MSG_ADDR_MDS(dis->get_asker()), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
	return 0;
  }

}




void MDCache::handle_inode_get_replica(MInodeGetReplica *m)
{
  CInode *in = get_inode(m->get_ino());
  if (!in) {
	dout(7) << "handle_inode_get_replica don't have inode for ino " << m->get_ino() << endl;
	assert(0);
	return;
  }

  dout(7) << "handle_inode_get_replica from " << m->get_source() << " for " << *in << endl;

  // add to cached_by
  in->cached_by_add(m->get_source());
  
  // add bit
  //****
  
  // reply
  mds->messenger->send_message(new MInodeGetReplicaAck(in->ino()),
							   MSG_ADDR_MDS(m->get_source()), MDS_PORT_CACHE, MDS_PORT_CACHE);

  // done.
  delete m;
}


void MDCache::handle_inode_get_replica_ack(MInodeGetReplicaAck *m)
{
  CInode *in = get_inode(m->get_ino());
  assert(in);

  dout(7) << "handle_inode_get_replica_ack from " << m->get_source() << " on " << *in << endl;

  // waiters
  list<Context*> finished;
  in->take_waiting(CINODE_WAIT_GETREPLICA, finished);
  for (list<Context*>::iterator it = finished.begin();
	   it != finished.end();
	   it++) {
	Context *c = *it;
	c->finish(0);
	delete c;
  }

  delete m;  
}






int MDCache::send_inode_updates(CInode *in)
{
  for (set<int>::iterator it = in->cached_by_begin(); 
	   it != in->cached_by_end(); 
	   it++) {
	dout(7) << "sending inode_update on " << *in << " to " << *it << endl;
	assert(*it != mds->get_nodeid());
	mds->messenger->send_message(new MInodeUpdate(in),
								 MSG_ADDR_MDS(*it), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
  }

  return 0;
}


void MDCache::handle_inode_update(MInodeUpdate *m)
{
  inodeno_t ino = m->get_ino();
  CInode *in = get_inode(m->get_ino());
  if (!in) {
	dout(7) << "got inode_update on " << m->get_ino() << ", don't have it, sending expire" << endl;

	mds->messenger->send_message(new MInodeExpire(m->get_ino(), mds->get_nodeid(), true),
								 m->get_source(), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
	
	delete m;
	return;
  }

  if (in->authority(mds->get_cluster()) == mds->get_nodeid()) {
	dout(7) << "got inode_update on " << *in << ", but i'm the authority!" << endl;
	delete m;
	return;
  }
  
  if (in->is_frozen()) {
	dout(7) << "got inode_update on " << *in << ", but i'm frozen, waiting. actually, this is pretty weird." << endl;	
	assert(0);
	CInode *parent = in->get_parent_inode();
	assert(parent);
	parent->dir->add_waiter(CDIR_WAIT_UNFREEZE,
							new C_MDS_RetryMessage(mds, m));
	return;
  }

  // update!
  dout(7) << "got inode_update on " << *in << endl;

  dout(7) << "dir_auth for " << *in << " was " << in->dir_auth << endl;

  // ugly hack to avoid corrupting weird behavior of dir_auth
  int old_dir_auth = in->dir_auth;
  bool wasours = in->dir_authority(mds->get_cluster()) == mds->get_nodeid();
  in->decode_basic_state(m->get_payload());
  bool isours = in->dir_authority(mds->get_cluster()) == mds->get_nodeid();
  if (wasours != isours)
	in->dir_auth = old_dir_auth;  // ignore dir_auth, it's clearly bogus
  
  dout(7) << "dir_auth for " << *in << " now " << in->dir_auth << " old " << old_dir_auth << " was/is " << wasours << " " << isours << endl;

  // done
  delete m;
}

void MDCache::handle_inode_expire(MInodeExpire *m)
{
  CInode *in = get_inode(m->get_ino());
  int from = m->get_from();
  int auth;

  if (!in) {
	dout(7) << "got inode_expire on " << m->get_ino() << " from " << from << ", don't have it" << endl;
	  
	goto forward;
  }

  auth = in->authority(mds->get_cluster());
  if (auth != mds->get_nodeid()) {
	dout(7) << "got inode_expire on " << *in << ", not mine" << endl;
	goto forward;
  }

  // remove from our cached_by
  if (!in->is_cached_by(from)) {
	dout(7) << "got inode_expire on " << *in << " from mds" << from << ", but they're not in cached_by "<< in->cached_by << endl;
	goto out;
  }

  dout(7) << "got inode_expire on " << *in << " from mds" << from << " cached_by now " << in->cached_by << endl;
  in->cached_by_remove(from);


  // done
 out:
  delete m;
  return;

  // ---------
 forward:
  if (m->is_soft()) {
	dout(7) << "got (soft) inode_expire on " << m->get_ino() << " from " << from << ", dropping" << endl;
	goto out;
  }

  if (m->get_hops() > mds->get_cluster()->get_num_mds()) {
	dout(5) << "dropping on floor." << endl;
	//assert(0);
	goto out;
  } else {
	dout(7) << "got inode_expire on " << m->get_ino() << " from mds" << from << ", fwding on, hops so far " << m->get_hops() << endl;
	m->add_hop();
	int next = mds->get_nodeid() + 1;
	if (next >= mds->get_cluster()->get_num_mds()) next = 0;
	mds->messenger->send_message(m,
								 MSG_ADDR_MDS(next), MDS_PORT_CACHE, MDS_PORT_CACHE);
	mds->logger->inc("iupfw");
  }
}


int MDCache::send_dir_updates(CDir *dir, int except)
{
  
  // FIXME

  int whoami = mds->get_nodeid();
  for (set<int>::iterator it = dir->inode->cached_by_begin(); 
	   it != dir->inode->cached_by_end(); 
	   it++) {
	if (*it == whoami) continue;
	if (*it == except) continue;
	dout(7) << "mds" << whoami << " sending dir_update on " << *(dir->inode) << " to " << *it << endl;
	mds->messenger->send_message(new MDirUpdate(dir->inode->inode.ino,
												dir->dir_rep,
												dir->dir_rep_by),
								 MSG_ADDR_MDS(*it), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
  }

  return 0;
}

void MDCache::handle_dir_update(MDirUpdate *m)
{
  CInode *in = get_inode(m->get_ino());
  if (!in) {
	dout(7) << "dir_update on " << m->get_ino() << ", don't have it" << endl;
	goto out;
  }

  // update!
  if (!in->dir) {
	dout(7) << "dropping dir_update on " << m->get_ino() << ", ->dir is null" << endl;	
	goto out;
  } 

  dout(7) << "dir_update on " << m->get_ino() << endl;
  
  in->dir->dir_rep = m->get_dir_rep();
  in->dir->dir_rep_by = m->get_dir_rep_by();

  // done
 out:
  delete m;
}






// locks ----------------------------------------------------------------

/*

INODES:

 two types of inode metadata:
  hard - uid/gid, mode
  soft - m/c/atime, size

 correspondingly, two types of locks:
  sync -  soft metadata.. no reads/writes can proceed.  (eg no stat)
  lock -  hard(+soft) metadata.. path traversals stop etc.  (??)


 replication consistency modes:
  hard+soft - hard and soft are defined on all replicas.
              all reads proceed (in absense of sync lock)
              writes require sync lock; possibly fw to auth
   -> normal behavior.

  hard      - hard only, soft is undefined
              reads require a sync
              writes proceed if field updates are monotonic (e.g. size, m/c/atime)
   -> 'softasync'

 types of access by cache users:

   hard   soft
    R      -    read_hard_try       path traversal
    R  <=  R    read_soft_start     stat
    R  <=  W    write_soft_start    touch
    W  =>  W    write_hard_start    chmod

   note on those implications:
     read_soft_start() calls read_hard_try()
     write_soft_start() calls read_hard_try()
     a hard lock implies/subsumes a soft sync  (read_soft_start() returns true if a lock is held)


 relationship with frozen directories:

   read_hard_try - can proceed, because any hard changes require a lock, which requires an active
      authority, which implies things are unfrozen.
   write_hard_start - waits (has to; only auth can initiate)
   read_soft_start  - ???? waits for now.  (FIXME: if !softasync & !syncbyauth)
   write_soft_start - ???? waits for now.  (FIXME: if (softasync & !syncbyauth))

   if sticky is on, an export_dir will drop any sync or lock so that the freeze will 
   proceed (otherwise, deadlock!).  likewise, a sync will not stick if is_freezing().
   


NAMESPACE:

 
*/


/* soft sync locks: mtime, size, etc. 
 */

bool MDCache::read_soft_start(CInode *in, Message *m)
{
  if (!read_hard_try(in, m))
	return false;

  // if frozen: i can't proceed (for now, see above)
  if (in->is_frozen()) {
	dout(7) << "read_soft_start " << *in << " is frozen, waiting" << endl;
	in->add_waiter(CDIR_WAIT_UNFREEZE,
				   new C_MDS_RetryMessage(mds, m));
	return false;
  }


  dout(5) << "read_soft_start " << *in << endl;

  // what soft sync mode?

  if (in->is_softasync()) {
	// softasync: hard consistency only

	if (in->is_auth()) {
	  // i am auth: i need sync
	  if (in->is_syncbyme()) return true;
	  if (in->is_lockbyme()) return true;   // lock => sync
	  if (!in->is_cached_by_anyone()) return true;  // i'm alone
	} else {
	  // i am replica: fw to auth
	  int auth = in->authority(mds->get_cluster());
	  dout(5) << "read_soft_start " << *in << " is softasync, fw to auth " << auth << endl;
	  assert(auth != mds->get_nodeid());
	  mds->messenger->send_message(m,
								   MSG_ADDR_MDS(auth), m->get_dest_port(),
								   MDS_PORT_CACHE);
	  return false;	  
	}
  } else {
	// normal: soft+hard consistency

	if (in->is_syncbyauth()) {
	  // wait for sync
	} else {
	  // i'm consistent 
	  return true;
	}
  }

  // we need sync
  if (in->is_syncbyauth() && !in->is_softasync()) 
    dout(5) << "read_soft_start " << *in << " is normal+replica+syncbyauth" << endl;
  else if (in->is_softasync() && in->is_auth())
    dout(5) << "read_soft_start " << *in << " is softasync+auth, waiting on sync" << endl;
  else 
	assert(2+2==5);

  if (!in->can_auth_pin()) {
	dout(5) << "read_soft_start " << *in << " waiting to auth_pin" << endl;
	in->add_waiter(CINODE_WAIT_AUTHPINNABLE,
				   new C_MDS_RetryMessage(mds,m));
	return false;
  }

  if (in->is_auth()) {
	// wait for sync
	in->add_waiter(CINODE_WAIT_SYNC,
				   new C_MDS_RetryMessage(mds, m));

	if (!in->is_presync())
	  sync_start(in);
  } else {
	// wait for unsync
	in->add_waiter(CINODE_WAIT_UNSYNC,
				   new C_MDS_RetryMessage(mds, m));

	assert(in->is_syncbyauth());

	if (!in->is_waitonunsync())
	  sync_wait(in);
  }
  
  return false;
}


int MDCache::read_soft_finish(CInode *in)
{
  dout(5) << "read_soft_finish " << *in << endl;   // " soft_sync_count " << in->soft_sync_count << endl;
  return 0;  // do nothing, actually..
}


bool MDCache::write_soft_start(CInode *in, Message *m)
{
  if (!read_hard_try(in, m))
	return false;

  // if frozen: i can't proceed (for now, see above)
  if (in->is_frozen()) {
	dout(7) << "read_soft_start " << *in << " is frozen, waiting" << endl;
	in->add_waiter(CDIR_WAIT_UNFREEZE,
				   new C_MDS_RetryMessage(mds, m));
	return false;
  }

  dout(5) << "write_soft_start " << *in << endl;
  // what soft sync mode?

  if (in->is_softasync()) {
	// softasync: hard consistency only

	if (in->is_syncbyauth()) {
	  // wait for sync release
	} else {
	  // i'm inconsistent; write away!
	  return true;
	}

  } else {
	// normal: soft+hard consistency
	
	if (in->is_auth()) {
	  // i am auth: i need sync
	  if (in->is_syncbyme()) return true;
	  if (in->is_lockbyme()) return true;   // lock => sync
	  if (!in->is_cached_by_anyone()) return true;  // i'm alone
	} else {
	  // i am replica: fw to auth
	  int auth = in->authority(mds->get_cluster());
	  dout(5) << "write_soft_start " << *in << " is !softasync, fw to auth " << auth << endl;
	  assert(auth != mds->get_nodeid());
	  mds->messenger->send_message(m,
								   MSG_ADDR_MDS(auth), m->get_dest_port(),
								   MDS_PORT_CACHE);
	  return false;	  
	}
  }

  // we need sync
  if (in->is_syncbyauth() && in->is_softasync() && !in->is_auth()) {
    dout(5) << "write_soft_start " << *in << " is softasync+replica+syncbyauth" << endl;
  } else if (!in->is_softasync() && in->is_auth())
    dout(5) << "write_soft_start " << *in << " is normal+auth, waiting on sync" << endl;
  else 
	assert(2+2==5);

  if (!in->can_auth_pin()) {
	dout(5) << "write_soft_start " << *in << " waiting to auth_pin" << endl;
	in->add_waiter(CINODE_WAIT_AUTHPINNABLE,
				   new C_MDS_RetryMessage(mds,m));
	return false;
  }

  if (in->is_auth()) {
	// wait for sync
	in->add_waiter(CINODE_WAIT_SYNC, 
				   new C_MDS_RetryMessage(mds, m));

	if (!in->is_presync())
	  sync_start(in);
  } else {
	// wait for unsync
	in->add_waiter(CINODE_WAIT_UNSYNC, 
				   new C_MDS_RetryMessage(mds, m));

	assert(in->is_syncbyauth());
	assert(in->is_softasync());
	
	if (!in->is_waitonunsync())
	  sync_wait(in);
  }
  
  return false;
}


int MDCache::write_soft_finish(CInode *in)
{
  dout(5) << "write_soft_finish " << *in << endl;  //" soft_sync_count " << in->soft_sync_count << endl;
  return 0;  // do nothing, actually..
}


// sync interface

void MDCache::sync_wait(CInode *in)
{
  assert(!in->is_auth());
  
  int auth = in->authority(mds->get_cluster());
  dout(5) << "sync_wait on " << *in << ", auth " << auth << endl;
  
  assert(in->is_syncbyauth());
  assert(!in->is_waitonunsync());
  
  in->dist_state |= CINODE_DIST_WAITONUNSYNC;
  in->get(CINODE_PIN_WAITONUNSYNC);
  
  if ((in->is_softasync() && g_conf.mdcache_sticky_sync_softasync) ||
	  (!in->is_softasync() && g_conf.mdcache_sticky_sync_normal)) {
	// actually recall; if !sticky, auth will immediately release.
	dout(5) << "sync_wait on " << *in << " sticky, recalling from auth" << endl;
	mds->messenger->send_message(new MInodeSyncRecall(in->inode.ino),
								 MSG_ADDR_MDS(auth), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
  }
}


void MDCache::sync_start(CInode *in)
{
  // wait for all replicas
  dout(5) << "sync_start on " << *in << ", waiting for " << in->cached_by << endl;

  assert(in->is_auth());
  assert(!in->is_presync());
  assert(!in->is_sync());

  in->sync_waiting_for_ack = in->cached_by;
  in->dist_state |= CINODE_DIST_PRESYNC;
  in->get(CINODE_PIN_PRESYNC);
  in->auth_pin();
  
  in->sync_replicawantback = false;

  // send messages
  for (set<int>::iterator it = in->cached_by_begin(); 
	   it != in->cached_by_end(); 
	   it++) {
	mds->messenger->send_message(new MInodeSyncStart(in->inode.ino, mds->get_nodeid()),
								 MSG_ADDR_MDS(*it), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
  }
}

void MDCache::sync_release(CInode *in)
{
  dout(5) << "sync_release on " << *in << ", messages to " << in->get_cached_by() << endl;
  
  assert(in->is_syncbyme());
  assert(in->is_auth());

  in->auth_unpin();
  in->dist_state &= ~CINODE_DIST_SYNCBYME;

  for (set<int>::iterator it = in->cached_by_begin(); 
	   it != in->cached_by_end(); 
	   it++) {
	mds->messenger->send_message(new MInodeSyncRelease(in),
								 MSG_ADDR_MDS(*it), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
  }
}


// messages
void MDCache::handle_inode_sync_start(MInodeSyncStart *m)
{
  // assume asker == authority for now.
  
  // authority is requesting a lock
  CInode *in = get_inode(m->get_ino());
  if (!in) {
	// don't have it anymore!
	dout(7) << "handle_sync_start " << m->get_ino() << ": don't have it anymore, nak" << endl;
	mds->messenger->send_message(new MInodeSyncAck(m->get_ino(), false),
								 MSG_ADDR_MDS(m->get_asker()), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
	delete m; // done
	return;
  }
  
  // we shouldn't be authoritative...
  assert(!in->is_auth());
  
  // open for write by clients?
  if (in->is_open_write()) {
	dout(7) << "handle_sync_start " << *in << " syncing write clients " << in->get_open_write() << endl;
	
	// sync clients
	in->client_wait_for_sync = in->open_write;
	for (multiset<int>::iterator it = in->get_open_write().begin();
		 it != in->get_open_write().end();
		 it++) {
	  mds->messenger->send_message(new MInodeSyncStart(in->ino(), mds->get_nodeid()),
								   MSG_ADDR_CLIENT(*it), 0,
								   MDS_PORT_CACHE);
	}

	in->pending_sync_request = m;	
  } else {
	// no writers, ack.
	dout(7) << "handle_sync_start " << *in << ", sending ack" << endl;
  
	inode_sync_ack(in, m);
  }
}

void MDCache::inode_sync_ack(CInode *in, MInodeSyncStart *m, bool wantback)
{
  dout(7) << "inode_sync_ack " << *in << endl;
  
  // lock it
  in->dist_state |= CINODE_DIST_SYNCBYAUTH;
  
  // send ack
  mds->messenger->send_message(new MInodeSyncAck(in->ino(), true, wantback),
							   MSG_ADDR_MDS(m->get_asker()), MDS_PORT_CACHE,
							   MDS_PORT_CACHE);

  delete m;
}

void MDCache::handle_inode_sync_ack(MInodeSyncAck *m)
{
  CInode *in = get_inode(m->get_ino());
  assert(in);

  if (MSG_ADDR_ISCLIENT(m->get_source())) {
	int client = MSG_ADDR_NUM(m->get_source());
	dout(7) << "handle_sync_ack from client " << client << " on " << *in << endl;
	
	assert(in->client_wait_for_sync.count(client) == 1);
	in->client_wait_for_sync.erase(in->client_wait_for_sync.find(client));
	if (in->client_wait_for_sync.empty()) {
	  inode_sync_ack(in, in->pending_sync_request, true);  // wantback!
	  in->pending_sync_request = 0;
	} else {
	  dout(7) << "handle_sync_ack still need clients " << in->client_wait_for_sync << endl;
	}

	delete m;
	return;
  }

  dout(7) << "handle_sync_ack " << *in << endl;

  assert(in->is_auth());
  assert(in->dist_state & CINODE_DIST_PRESYNC);

  // remove it from waiting list
  in->sync_waiting_for_ack.erase(m->get_source());
  
  if (!m->did_have()) {
	// erase from cached_by too!
	in->cached_by_remove(m->get_source());
  }

  if (m->replica_wantsback())
	in->sync_replicawantback = true;

  if (in->sync_waiting_for_ack.size()) {

	// more coming
	dout(7) << "handle_sync_ack " << *in << " from " << m->get_source() << ", still waiting for " << in->sync_waiting_for_ack << endl;
	
  } else {
	
	// yay!
	dout(7) << "handle_sync_ack " << *in << " from " << m->get_source() << ", last one" << endl;

	in->dist_state &= ~CINODE_DIST_PRESYNC;
	in->dist_state |= CINODE_DIST_SYNCBYME;
	in->put(CINODE_PIN_PRESYNC);

	// do waiters!
	list<Context*> finished;
	in->take_waiting(CINODE_WAIT_SYNC, finished);

	for (list<Context*>::iterator it = finished.begin();
		 it != finished.end();
		 it++) {
	  Context *c = *it;
	  if (c) {
		c->finish(0);
		delete c;
	  }
	}

	// release sync right away?
	if (in->is_freezing()) {
	  dout(7) << "handle_sync_ack freezing " << *in << ", dropping sync immediately" << endl;
	  sync_release(in);
	} 
	else if (in->sync_replicawantback) {
	  dout(7) << "handle_sync_ack replica wantback, releasing sync immediately" << endl;
	  sync_release(in);
	}
	else if ((in->is_softasync() && !g_conf.mdcache_sticky_sync_softasync) ||
			 (!in->is_softasync() && !g_conf.mdcache_sticky_sync_normal)) {
	  dout(7) << "handle_sync_ack !sticky, releasing sync immediately" << endl;
	  sync_release(in);
	} 
	else 
	  dout(7) << "handle_sync_ack sticky sync is on, keeping sync for now" << endl;
  }

  delete m; // done
}


void MDCache::handle_inode_sync_release(MInodeSyncRelease *m)
{
  CInode *in = get_inode(m->get_ino());

  if (!in) {
	dout(7) << "handle_sync_release " << m->get_ino() << ", don't have it, dropping" << endl;
	delete m;  // done
	return;
  }
  
  if (!in->is_syncbyauth()) {
	dout(7) << "handle_sync_release " << m->get_ino() << ", not flagged as sync, dropping" << endl;
	assert(0);
	delete m;  // done
	return;
  }
  
  dout(7) << "handle_sync_release " << *in << endl;
  assert(!in->is_auth());
  
  // release state
  in->dist_state &= ~CINODE_DIST_SYNCBYAUTH;

  // waiters?
  if (in->is_waitonunsync()) {
	in->put(CINODE_PIN_WAITONUNSYNC);
	in->dist_state &= ~CINODE_DIST_WAITONUNSYNC;

	// finish
	list<Context*> finished;
	in->take_waiting(CINODE_WAIT_UNSYNC, finished);
	for (list<Context*>::iterator it = finished.begin(); 
		 it != finished.end(); 
		 it++) {
	  Context *c = *it;
	  c->finish(0);
	  delete c;
	}
  }

  // client readers?
  if (in->is_open_write()) {
	dout(7) << "handle_sync_release releasing clients " << in->get_open_write() << endl;
	for (multiset<int>::iterator it = in->get_open_write().begin();
		 it != in->get_open_write().end();
		 it++) {
	  mds->messenger->send_message(new MInodeSyncRelease(in),
								   MSG_ADDR_CLIENT(*it), 0,
								   MDS_PORT_CACHE);
	}
  }

  
  // done
  delete m;
}


void MDCache::handle_inode_sync_recall(MInodeSyncRecall *m)
{
  CInode *in = get_inode(m->get_ino());

  if (!in) {
	dout(7) << "handle_sync_recall " << m->get_ino() << ", don't have it, dropping" << endl;
	delete m;  // done
	return;
  }
  
  if (!in->is_syncbyme()) {
	dout(7) << "handle_sync_recall " << m->get_ino() << ", not flagged as sync, dropping" << endl;
	delete m;  // done
	return;
  }
  
  dout(7) << "handle_sync_recall " << *in << ", releasing" << endl;
  assert(in->is_auth());
  
  sync_release(in);
  
  // done
  delete m;
}




/* hard locks: owner, mode 
 */

bool MDCache::read_hard_try(CInode *in,
							Message *m)
{
  //dout(5) << "read_hard_try " << *in << endl;
  
  if (in->is_auth()) {
	// auth
	return true;      // fine
  } else {
	// replica
	if (in->is_lockbyauth()) {
	  // locked by auth; wait!
	  dout(7) << "read_hard_try waiting on " << *in << endl;
	  in->add_waiter(CINODE_WAIT_UNLOCK, new C_MDS_RetryMessage(mds, m));
	  if (!in->is_waitonunlock())
		inode_lock_wait(in);
	  return false;
	} else {
	  // not locked.
	  return true;
	}
  }
}


bool MDCache::write_hard_start(CInode *in, 
							   Message *m)
{
  // if frozen: i can't proceed; only auth can initiate lock
  if (in->is_frozen()) {
	dout(7) << "write_hard_start " << *in << " is frozen, waiting" << endl;
	in->add_waiter(CDIR_WAIT_UNFREEZE,
				   new C_MDS_RetryMessage(mds, m));
	return false;
  }


  if (in->is_auth()) {
	// auth
	if (in->is_lockbyme()) goto success;
	if (!in->is_cached_by_anyone()) goto success;
	
	// need lock
	if (!in->can_auth_pin()) {
	  dout(5) << "write_hard_start " << *in << " waiting to auth_pin" << endl;
	  in->add_waiter(CINODE_WAIT_AUTHPINNABLE, new C_MDS_RetryMessage(mds, m));
	  return false;
	}
	
	in->add_waiter(CINODE_WAIT_LOCK, new C_MDS_RetryMessage(mds, m));
	in->lock_active_count++;
	
	if (!in->is_prelock())
	  inode_lock_start(in);
	
	return false;
  } else {
	// replica
	// fw to auth
	int auth = in->authority(mds->get_cluster());
	dout(5) << "write_hard_start " << *in << " on replica, fw to auth " << auth << endl;
	assert(auth != mds->get_nodeid());
	mds->messenger->send_message(m,
								 MSG_ADDR_MDS(auth), m->get_dest_port(),
								 MDS_PORT_CACHE);
	return false;
  }

 success:
  in->lock_active_count++;
  assert(in->lock_active_count > 0);
}

void MDCache::write_hard_finish(CInode *in)
{
  dout(5) << "write_hard_finish " << *in << " count " << in->lock_active_count << endl;

  assert(in->lock_active_count > 0);
  in->lock_active_count--;

  // release lock?
  if (in->lock_active_count == 0 &&
	  in->is_lockbyme() &&
	  !g_conf.mdcache_sticky_lock) {
	dout(7) << "write_hard_finish " << *in << " !sticky, releasing lock immediately" << endl;
	inode_lock_release(in);
  }
}


void MDCache::inode_lock_start(CInode *in)
{
  dout(5) << "lock_start on " << *in << ", waiting for " << in->cached_by << endl;

  assert(in->is_auth());
  assert(!in->is_prelock());
  assert(!in->is_lockbyme());
  assert(!in->is_lockbyauth());

  in->lock_waiting_for_ack = in->cached_by;
  in->dist_state |= CINODE_DIST_PRELOCK;
  in->get(CINODE_PIN_PRELOCK);
  in->auth_pin();

  // send messages
  for (set<int>::iterator it = in->cached_by_begin(); 
	   it != in->cached_by_end(); 
	   it++) {
	mds->messenger->send_message(new MInodeLockStart(in->inode.ino, mds->get_nodeid()),
								 MSG_ADDR_MDS(*it), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
  }
}


void MDCache::inode_lock_release(CInode *in)
{
  dout(5) << "lock_release on " << *in << ", messages to " << in->get_cached_by() << endl;
  
  assert(in->is_lockbyme());
  assert(in->is_auth());

  in->auth_unpin();
  in->dist_state &= ~CINODE_DIST_LOCKBYME;

  for (set<int>::iterator it = in->cached_by_begin(); 
	   it != in->cached_by_end(); 
	   it++) {
	mds->messenger->send_message(new MInodeLockRelease(in),
								 MSG_ADDR_MDS(*it), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
  }
}

void MDCache::inode_lock_wait(CInode *in)
{
  dout(5) << "lock_wait on " << *in << endl;
  assert(!in->is_auth());
  assert(in->is_lockbyauth());
  
  in->dist_state |= CINODE_DIST_WAITONUNLOCK;
  in->get(CINODE_PIN_WAITONUNLOCK);
}


void MDCache::handle_inode_lock_start(MInodeLockStart *m)
{
  // authority is requesting a lock
  CInode *in = get_inode(m->get_ino());
  if (!in) {
	// don't have it anymore!
	dout(7) << "handle_lock_start " << m->get_ino() << ": don't have it anymore, nak" << endl;
	mds->messenger->send_message(new MInodeLockAck(m->get_ino(), false),
								 MSG_ADDR_MDS(m->get_asker()), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
	delete m; // done
	return;
  }
  
  // we shouldn't be authoritative...
  assert(!in->is_auth());
  
  dout(7) << "handle_lock_start " << *in << ", sending ack" << endl;
  
  // lock it
  in->dist_state |= CINODE_DIST_LOCKBYAUTH;
  
  // send ack
  mds->messenger->send_message(new MInodeLockAck(in->ino()),
							   MSG_ADDR_MDS(m->get_asker()), MDS_PORT_CACHE,
							   MDS_PORT_CACHE);

  delete m;  // done
}


void MDCache::handle_inode_lock_ack(MInodeLockAck *m)
{
  CInode *in = get_inode(m->get_ino());
  int from = m->get_source();
  dout(7) << "handle_lock_ack from " << from << " on " << *in << endl;

  assert(in);
  assert(in->is_auth());
  assert(in->dist_state & CINODE_DIST_PRELOCK);

  // remove it from waiting list
  in->lock_waiting_for_ack.erase(from);
  
  if (!m->did_have()) {
	// erase from cached_by too!
	in->cached_by_remove(from);
  }

  if (in->lock_waiting_for_ack.size()) {

	// more coming
	dout(7) << "handle_lock_ack " << *in << " from " << from << ", still waiting for " << in->lock_waiting_for_ack << endl;
	
  } else {
	
	// yay!
	dout(7) << "handle_lock_ack " << *in << " from " << from << ", last one" << endl;

	in->dist_state &= ~CINODE_DIST_PRELOCK;
	in->dist_state |= CINODE_DIST_LOCKBYME;
	in->put(CINODE_PIN_PRELOCK);

	// do waiters!
	list<Context*> finished;
	in->take_waiting(CINODE_WAIT_LOCK, finished);

	for (list<Context*>::iterator it = finished.begin();
		 it != finished.end();
		 it++) {
	  in->lock_active_count--;  // effectively dequeued
	  Context *c = *it;
	  if (c) {
		c->finish(0);
		delete c;
	  }
	}
  }

  delete m; // done
}


void MDCache::handle_inode_lock_release(MInodeLockRelease *m)
{
  CInode *in = get_inode(m->get_ino());

  if (!in) {
	dout(7) << "handle_lock_release " << m->get_ino() << ", don't have it, dropping" << endl;
	delete m;  // done
	return;
  }
  
  if (!in->is_lockbyauth()) {
	dout(7) << "handle_lock_release " << m->get_ino() << ", not flagged as locked, dropping" << endl;
	assert(0);   // i should have it, locked, or not have it at all!
	delete m;  // done
	return;
  }
  
  dout(7) << "handle_lock_release " << *in << endl;
  assert(!in->is_auth());
  
  // release state
  in->dist_state &= ~CINODE_DIST_LOCKBYAUTH;

  // waiters?
  if (in->is_waitonunlock()) {
	in->put(CINODE_PIN_WAITONUNLOCK);
	in->dist_state &= ~CINODE_DIST_WAITONUNLOCK;
	
	// finish
	list<Context*> finished;
	in->take_waiting(CINODE_WAIT_UNLOCK, finished);
	for (list<Context*>::iterator it = finished.begin(); 
		 it != finished.end(); 
		 it++) {
	  Context *c = *it;
	  c->finish(0);
	  delete c;
	}
  }
  
  // done
  delete m;
}










// IMPORT/EXPORT

class C_MDS_ExportFreeze : public Context {
  MDS *mds;
  CInode *in;   // inode of dir i'm exporting
  int dest;
  double pop;

public:
  C_MDS_ExportFreeze(MDS *mds, CInode *in, int dest, double pop) {
	this->mds = mds;
	this->in = in;
	this->dest = dest;
	this->pop = pop;
  }
  virtual void finish(int r) {
	mds->mdcache->export_dir_frozen(in, dest, pop);
  }
};

class C_MDS_ExportFinish : public Context {
  MDS *mds;
  CInode *in;   // inode of dir i'm exporting

public:
  // contexts for waiting operations on the affected subtree
  list<Context*> will_redelegate;
  list<Context*> will_fail;

  C_MDS_ExportFinish(MDS *mds, CInode *in) {
	this->mds = mds;
	this->in = in;
  }

  // suck up and categorize waitlists 
  void assim_waitlist(list<Context*>& ls) {
	for (list<Context*>::iterator it = ls.begin();
		 it != ls.end();
		 it++) {
	  dout(7) << "assim_waitlist context " << *it << endl;
	  if ((*it)->can_redelegate()) 
		will_redelegate.push_back(*it);
	  else
		will_fail.push_back(*it);
	}
	ls.clear();
  }
  void assim_waitlist(hash_map< string, list<Context*> >& cmap) {
	for (hash_map< string, list<Context*> >::iterator hit = cmap.begin();
		 hit != cmap.end();
		 hit++) {
	  for (list<Context*>::iterator lit = hit->second.begin(); lit != hit->second.end(); lit++) {
		dout(7) << "assim_waitlist context " << *lit << endl;
		if ((*lit)->can_redelegate()) 
		  will_redelegate.push_back(*lit);
		else
		  will_fail.push_back(*lit);
	  }
	}
	cmap.clear();
  }


  virtual void finish(int r) {
	if (r >= 0) { 
	  // redelegate
	  list<Context*>::iterator it;
	  for (it = will_redelegate.begin(); it != will_redelegate.end(); it++) {
		(*it)->redelegate(mds, in->dir_authority(mds->get_cluster()));
		delete *it;  // delete context
	  }

	  // fail
	  // this happens with: 
	  // - commit_dir
	  // - ?
	  for (it = will_fail.begin(); it != will_fail.end(); it++) {
		Context *c = *it;
		dout(7) << "failing context " << c << endl;
		//assert(false);
		c->finish(-1);  // fail
		delete c;   // delete context
	  }	  
	} else {
	  assert(false); // now what?
	}
  }
};


void MDCache::export_dir(CInode *in,
						 int dest)
{
  assert(dest != mds->get_nodeid());

  if (!in->dir) in->dir = new CDir(in, mds->get_nodeid());

  if (!in->parent) {
	dout(7) << "i won't export root" << endl;
	assert(in->parent);
	return;
  }

  if (in->dir->is_frozen() ||
	  in->dir->is_freezing()) {
	dout(7) << " can't export, freezing|frozen.  wait for other exports to finish first." << endl;
	return;
  }

  // send ExportDirPrep (ask target)
  dout(7) << "export_dir " << *in << " to " << dest << ", sending ExportDirPrep" << endl;
  mds->messenger->send_message(new MExportDirPrep(in),
							   dest, MDS_PORT_CACHE, MDS_PORT_CACHE);
  in->dir->auth_pin();   // pin dir, to hang up our freeze
  mds->logger->inc("ex");

  // take away popularity (and pass it on to the context, MExportDir request later)
  double pop = in->popularity.get();
  CInode *t = in;
  while (t) {
	t->popularity.adjust(-pop);
	if (t->parent)
	  t = t->parent->dir->inode;
	else 
	  break;
  }

  // freeze the subtree
  //dout(7) << "export_dir " << *in << " to " << dest << ", freezing" << endl;
  in->dir->freeze_tree(new C_MDS_ExportFreeze(mds, in, dest, pop));

  // drop any sync or lock if sticky
  if (g_conf.mdcache_sticky_sync_normal ||
	  g_conf.mdcache_sticky_sync_softasync)
	export_dir_dropsync(in);
}

void MDCache::export_dir_dropsync(CInode *idir)
{
  assert(idir->is_dir());
  if (!idir->dir)
	return;  // we don't ahve anything, obviously

  CDir_map_t::iterator it;
  for (it = idir->dir->begin(); it != idir->dir->end(); it++) {
	CInode *in = it->second->inode;

	dout(7) << "about to export: dropping sticky(?) sync on " << *in << endl;
	if (in->is_syncbyme()) sync_release(in);

	if (in->is_dir() &&
		in->dir_auth == CDIR_AUTH_PARENT &&      // mine
		in->nested_auth_pins > 0)  // might be sync
	  export_dir_dropsync(in);
  }
}



void MDCache::handle_export_dir_prep_ack(MExportDirPrepAck *m)
{
  CInode *in = get_inode(m->get_ino());
  assert(in);

  dout(7) << "export_dir_prep_ack " << *in << ", releasing auth_pin" << endl;
  
  in->dir->auth_unpin();   // unpin to allow freeze to complete

  // done
  delete m;
}


void MDCache::export_dir_frozen(CInode *in,
								int dest,
								double pop)
{
  // subtree is now frozen!
  dout(7) << "export_dir " << *in << " to " << dest << ", frozen+prep_ack" << endl;

  show_imports();

  
  // update imports/exports
  CInode *containing_import = get_containing_import(in);
  if (containing_import == in) {
	dout(7) << " i'm rexporting a previous import" << endl;
	imports.erase(in);
	in->dir->state_clear(CDIR_STATE_IMPORT);
	in->put(CINODE_PIN_IMPORT);                  // unpin, no longer an import

	// discard nested exports (that we're handing off
	pair<multimap<CInode*,CInode*>::iterator, multimap<CInode*,CInode*>::iterator> p =
	  nested_exports.equal_range(in);
	while (p.first != p.second) {
	  CInode *nested = (*p.first).second;

	  // nested beneath our new export *in; remove!
	  dout(7) << " export " << *nested << " was nested beneath us; removing from export list(s)" << endl;
	  assert(exports.count(nested) == 1);
	  //exports.erase(nested);  _walk does this
	  nested_exports.erase(p.first++);   // note this increments before call to erase
	}

  } else {
	dout(7) << " i'm a subdir nested under import " << *containing_import << endl;
	exports.insert(in);
	nested_exports.insert(pair<CInode*,CInode*>(containing_import, in));

	in->get(CINODE_PIN_EXPORT);                  // i must keep it pinned
	
	// discard nested exports (that we're handing off)
	pair<multimap<CInode*,CInode*>::iterator, multimap<CInode*,CInode*>::iterator> p =
	  nested_exports.equal_range(containing_import);
	while (p.first != p.second) {
	  CInode *nested = (*p.first).second;
	  multimap<CInode*,CInode*>::iterator prev = p.first;
	  p.first++;

	  CInode *containing_export = get_containing_export(nested->get_parent_inode());
	  if (!containing_export) continue;
	  if (nested == in) continue;  // ignore myself

	  if (containing_export == in) {
		// nested beneath our new export *in; remove!
		dout(7) << " export " << *nested << " was nested beneath us; removing from nested_exports" << endl;
		// exports.erase(nested); _walk does this
		nested_exports.erase(prev);  // note this increments before call to erase
	  } else {
		dout(7) << " huh, other export " << *nested << " is under export " << *containing_export << ", which is odd" << endl;
		assert(0);
	  }

	}

  }

  // note new authority (locally)
  in->dir_auth = dest;
  if (in->parent &&
	  in->get_parent_inode()->dir_auth == in->dir_auth)
	in->dir_auth = CDIR_AUTH_PARENT;

  // build export message
  MExportDir *req = new MExportDir(in, pop);  // include pop
  
  // fill with relevant cache data
  C_MDS_ExportFinish *fin = new C_MDS_ExportFinish(mds, in);

  export_dir_walk( req, fin, in, dest );

  mds->messenger->send_message(req,
							   MSG_ADDR_MDS(dest), MDS_PORT_CACHE,
							   MDS_PORT_CACHE);

  // queue finisher
  in->dir->add_waiter( CDIR_WAIT_UNFREEZE, fin );
}

void MDCache::export_dir_walk(MExportDir *req,
							  C_MDS_ExportFinish *fin,
							  CInode *idir,
							  int newauth)
{
  assert(idir->is_dir());
  if (!idir->dir)
	return;  // we don't ahve anything, obviously

  dout(7) << "export_dir_walk on " << *idir << " " << idir->dir->nitems << " items" << endl;

  // dir 
  crope dir_rope;

  Dir_Export_State_t dstate;
  dstate.ino = idir->inode.ino;
  dstate.nitems = idir->dir->nitems;
  dstate.version = idir->dir->version;
  dstate.state = idir->dir->state;
  dstate.dir_rep = idir->dir->dir_rep;
  dstate.ndir_rep_by = idir->dir->dir_rep_by.size();
  dstate.popularity = idir->dir->popularity;
  dir_rope.append( (char*)&dstate, sizeof(dstate) );
  
  for (set<int>::iterator it = idir->dir->dir_rep_by.begin();
	   it != idir->dir->dir_rep_by.end();
	   it++) {
	int i = *it;
	dir_rope.append( (char*)&i, sizeof(int) );
  }

  // mark
  assert(idir->dir->is_auth());
  idir->dir->state_clear(CDIR_STATE_AUTH);

  // discard most dir state
  idir->dir->state &= CDIR_MASK_STATE_EXPORT_KEPT;  // i only retain a few things.

  // waiters
  list<Context*> waiting;
  idir->take_waiting(CINODE_WAIT_ANY, waiting);    // FIXME
  fin->assim_waitlist(waiting);


  // inodes
  list<CInode*> subdirs;

  CDir_map_t::iterator it;
  for (it = idir->dir->begin(); it != idir->dir->end(); it++) {
	CInode *in = it->second->inode;

	in->version++;  // so log entries are ignored, etc.

	// idir hashed?  make dir_auth explicit
	if (idir->dir->is_hashed() &&
		in->dir_auth == CDIR_AUTH_PARENT)
	  in->dir_auth = mds->get_nodeid();
	
	// if hashed, only include dirs
	if (in->is_dir() || !idir->dir->is_hashed()) {
	  // dentry
	  dir_rope.append( it->first.c_str(), it->first.length()+1 );
	  
	  // add inode
	  dir_rope.append( in->encode_export_state() );
	}
	
	if (in->is_dir()) {
	  assert(in->dir_auth != mds->get_nodeid() || in->dir_is_hashed()); // shouldn't refer to us explicitly!

	  if (in->dir_is_hashed()) {
		dout(7) << " encountered hashed dir " << *in << endl;
		assert(!in->dir || in->dir->is_hashed());
	  } else 
		assert(!in->dir || !in->dir->is_hashed());
	  
	  if (in->dir_auth == CDIR_AUTH_PARENT ||
		  (in->dir_is_hashed() && in->dir_auth == mds->get_nodeid())) {
		subdirs.push_back(in);  // it's ours, recurse.
	  } else {
		dout(7) << " encountered nested export " << *in << " dir_auth " << in->dir_auth << "; removing from exports" << endl;
		assert(exports.count(in) == 1); 
		exports.erase(in);                    // discard nested export   (nested_exports updated above)
		in->put(CINODE_PIN_EXPORT);
	  }
	} 

	// we don't export inodes if hashed
	if (idir->dir->is_hashed()) {
	  // but we do replicate all dirs on new auth
	  if (in->is_dir()) {
		if (in->is_auth()) {
		  // it's mine, easy enough: new auth will replicate my inode
		  if (!in->is_cached_by(newauth))
			in->cached_by_add( newauth );
		} 
		else {
		  // hmm, i'm a replica.  they'll need to contact auth themselves if they don't already have it!
		}
	  }
	} else {
	  // we're export this inode; fix inode state

	  if (in->is_dirty()) in->mark_clean();

	  // clear/unpin cached_by (we're no longer the authority)
	  in->cached_by_clear();
	  
	  // mark auth
	  assert(in->auth == true);
	  in->auth = false;
	  
	  // *** other state too?
	  
	  // waiters
	  list<Context*> waiters;
	  idir->dir->take_waiting(CDIR_WAIT_ANY, waiters);
	  fin->assim_waitlist(waiters);
	}
  }

  req->add_dir( dir_rope );
  
  // subdirs
  for (list<CInode*>::iterator it = subdirs.begin(); it != subdirs.end(); it++)
	export_dir_walk(req, fin, *it, newauth);
}


void MDCache::handle_export_dir_ack(MExportDirAck *m)
{
  // exported!
  CInode *in = mds->mdcache->get_inode(m->get_ino());
  int newauth = m->get_source();

  dout(7) << "export_dir_ack " << *in << endl;
  
  // remove the metadata from the cache
  //no, borken! if (in->dir) export_dir_purge( in, newauth );


  // FIXME log it

  // unfreeze
  dout(7) << "export_dir_ack " << *in << ", unfreezing" << endl;
  in->dir->unfreeze_tree();

  show_imports();

  // done
  delete m;
}


// called by handle_expirt_dir_ack
void MDCache::export_dir_purge(CInode *idir, int newauth)
{
  dout(7) << "export_dir_purge on " << *idir << endl;

  assert(0);
  /**

  BROKEN:  in order for this to work we need to know what the bounds (re-exports were) at 
  the time of the original export, so that we can only deal with those entries; however, we
  don't know what those items are because we deleted them from exports lists earlier.

  so, we don't explicitly purge.  instead, exported items expire from the cache normally.

  **/

  // discard most dir state
  //idir->dir->state &= CDIR_MASK_STATE_EXPORT_KEPT;  // i only retain a few things.
  
  assert(!idir->dir->is_auth());

  // contents:
  CDir_map_t::iterator it = idir->dir->begin();
  while (it != idir->dir->end()) {
	CInode *in = it->second->inode;
	it++;
	
	assert(in->auth == false);
	
	if (in->is_dir() && in->dir) 
	  export_dir_purge(in, newauth);
	
	dout(7) << "sending inode_expire to mds" << newauth << " on " << *in << endl;
	mds->messenger->send_message(new MInodeExpire(in->inode.ino, mds->get_nodeid()),
								 MSG_ADDR_MDS(newauth), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
	
	if (in->lru_expireable) {
	  lru->lru_remove(in);
	  dout(7) << "export_dir_purge deleting " << *in << " " << in << endl;
	  remove_inode(in);
	  delete in;
	} else {
	  dout(7) << "export_dir_purge not deleting non-expireable " << *in << " " << in->ref_set << endl;
	}
  }

  dout(7) << "export_dir_purge on " << *idir << " done" << endl;
}







//  IMPORTS

void MDCache::handle_export_dir_prep(MExportDirPrep *m)
{
  dout(7) << "handle_export_dir_prep on " << m->get_path() << endl;

  assert(m->get_source() != mds->get_nodeid());

  // must discover it!
  vector<CInode*> trav;

  int r = path_traverse(m->get_path(), trav, m, MDS_TRAVERSE_DISCOVER);   
  if (r > 0) return;  // fw or delay
  
  // okay
  CInode *in = trav[trav.size()-1];

  if (!in->dir) in->dir = new CDir(in, mds->get_nodeid());
  assert(in->dir->is_auth() == false);

  in->dir->auth_pin();     // auth_pin until we get the data
  
  dout(7) << "sending export_dir_prep_ack on " << *in << endl;
  
  mds->messenger->send_message(new MExportDirPrepAck(in->ino()),
							   m->get_source(), MDS_PORT_CACHE, MDS_PORT_CACHE);
  
  // done 
  delete m;
}




/* this guy waits for the discover to finish.  if it's the last one on the dir,
 * it unfreezes it.  if it's the last frozen hashed dir, it triggers the above.
 */
class C_MDS_ImportHashedReplica : public Context {
public:
  MDS *mds;
  MExportDir *m;
  CInode *in;
  inodeno_t dir_ino, replica_ino;
  C_MDS_ImportHashedReplica(MDS *mds, CInode *in, inodeno_t dir_ino, inodeno_t replica_ino) {
	this->mds = mds;
	this->in = in;
	this->dir_ino = dir_ino;
	this->replica_ino = replica_ino;
  }
  virtual void finish(int r) {
	assert(r == 0);  // should never fail!
	mds->mdcache->got_hashed_replica(in, dir_ino, replica_ino);
  }
};




void MDCache::handle_export_dir(MExportDir *m)
{
  CInode *in = get_inode(m->get_ino());
  int oldauth = m->get_source();
  assert(in);
  
  dout(7) << "handle_export_dir, import_dir " << *in << endl;

  show_imports();

  mds->logger->inc("im");

  if (!in->dir) in->dir = new CDir(in, mds->get_nodeid());

  assert(in->dir->is_auth() == false);

  // note new authority (locally)
  in->dir_auth = mds->get_nodeid();

  CInode *containing_import;
  if (exports.count(in)) {
	// reimporting
	dout(7) << " i'm reimporting this dir!" << endl;
	exports.erase(in);

	in->put(CINODE_PIN_EXPORT);                // unpin, no longer an export

	containing_import = get_containing_import(in);  
	dout(7) << "  it is nested under import " << *containing_import << endl;
	for (pair< multimap<CInode*,CInode*>::iterator, multimap<CInode*,CInode*>::iterator > p =
		   nested_exports.equal_range( containing_import );
		 p.first != p.second;
		 p.first++) {
	  if ((*p.first).second == in) {
		nested_exports.erase(p.first);
		break;
	  }
	}
  } else {
	// new import
	imports.insert(in);
	in->dir->state_set(CDIR_STATE_IMPORT);
	in->get(CINODE_PIN_IMPORT);                // must keep it pinned

	containing_import = in;  // imported exports nested under *in
  }

  // i shouldn't be waiting for any ReplicateHashedAck's yet
  assert(import_hashed_replicate_waiting.count(m->get_ino()) == 0);

  // add this crap to my cache
  const char *p = m->get_state().c_str();
  for (int i = 0; i < m->get_ndirs(); i++) 
	import_dir_block(p, containing_import, oldauth, in);
  
  // can i simplify dir_auth?
  if (in->authority(mds->get_cluster()) == in->dir_auth)
	in->dir_auth = CDIR_AUTH_PARENT;

  double newpop = m->get_ipop() - in->popularity.get();
  dout(7) << " imported popularity jump by " << newpop << endl;
  if (newpop > 0) {  // duh
	CInode *t = in;
	while (t) {
	  t->popularity.adjust(newpop);
	  if (t->parent) 
		t = t->parent->dir->inode;
	  else break;
	}
  }

  // send ack
  dout(7) << "sending ack back to " << m->get_source() << endl;
  MExportDirAck *ack = new MExportDirAck(m);
  mds->messenger->send_message(ack,
							   m->get_source(), MDS_PORT_CACHE,
							   MDS_PORT_CACHE);

  // done
  delete m;


  // FIXME LOG IT


  // wait for replicas in hashed dirs?
  if (import_hashed_replicate_waiting.count(m->get_ino())) {
	// it'll happen later!, when i get my inodegetreplicaack's back
  } else {
	// finish now
	handle_export_dir_finish(in);
  }
}


void MDCache::handle_export_dir_finish(CInode *in)
{
  assert(in->dir->is_auth());
  
  // spread the word!
  if (in->authority(mds->get_cluster()) == mds->get_nodeid()) {
	// i am the authority
	send_inode_updates(in);
  } else {
	// tell the authority; they'll spread the word.
	string path;
	in->make_path(path);
	mds->messenger->send_message(new MExportDirNotify(path, in->dir_auth),
								 MSG_ADDR_MDS(in->authority(mds->get_cluster())), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
  }

  in->dir->auth_unpin();  

  dout(5) << "done with import!" << endl;
  show_imports();
  mds->logger->set("nex", exports.size());
  mds->logger->set("nim", imports.size());





  // finish contexts
  dout(5) << "finishing any waiters on imported data" << endl;
  list<Context*> finished;
  in->dir->take_waiting(CDIR_WAIT_IMPORTED, finished);
  for (list<Context*>::iterator it = finished.begin();
	   it != finished.end();
	   it++) {
	Context *c = *it;
	if (c) {
	  c->finish(0);
	  delete c;
	}
  }
}


CInode *MDCache::import_dentry_inode(CDir *dir,
									 pchar& p,
									 int from,
									 CInode *import_root)
{
  // we have three cases:
  assert((dir->is_auth() && !dir->is_hashing()) ||  // auth importing (may be hashed or normal)
		 (!dir->is_auth() && dir->is_hashing()) ||  // nonauth hashing (not yet hashed)
		 (dir->is_auth() && dir->is_unhashing()));  // auth reassimilating (currently hashed)
  
  // dentry
  string dname = p;
  p += dname.length()+1;
  
  // inode
  Inode_Export_State_t *istate = (Inode_Export_State_t*)p;
  p += sizeof(*istate);

  CInode *in = get_inode(istate->inode.ino);
  bool importing = true;
  bool had_inode = false;
  if (!in) {
	in = new CInode;
	in->inode = istate->inode;

	// add
	add_inode(in);
	link_inode(dir->inode, dname, in);	
	dout(7) << "   import_dentry_inode adding " << *in << " istate.dir_auth " << istate->dir_auth << endl;
  } else {
	dout(7) << "   import_dentry_inode already had " << *in << " istate.dir_auth " << istate->dir_auth << endl;
	had_inode = true;
  }

  // auth wonkiness
  if (dir->is_unhashing()) {
	// auth reassimilating
	in->inode = istate->inode;
	in->auth = true;
  } 
  else if (dir->is_hashed()) {
	// import on hashed dir
	assert(in->is_dir());
	if (in->authority(mds->get_cluster()) == mds->get_nodeid())
	  in->auth = true;
	else 
	  in->auth = false;
	importing = false;
  } 
  else {
	// normal import
	in->inode = istate->inode;
	in->auth = true;
  }
  

  // assimilate new auth state?
  if (importing) {

	// dir_auth (for dirs underneath me)
	in->dir_auth = istate->dir_auth;
	if (in->dir_is_hashed()) {
	  dout(7) << "   imported hashed dir " << *in << endl;
	  assert(!in->dir || in->dir->is_hashed());
	} else
	  assert(!in->dir || !in->dir->is_hashed());


	// update inode state with authoritative info
	in->version = istate->version;
	in->popularity = istate->popularity;

	// cached_by
	in->cached_by.clear(); 
	for (int nby = istate->ncached_by; nby>0; nby--) {
	  if (*((int*)p) != mds->get_nodeid()) 
		in->cached_by_add( *((int*)p) );
	  p += sizeof(int);
	}
	
	in->cached_by_add(from);             // old auth still has it too.
  
	// dist state: new authority inherits softasync state only; sync/lock are dropped for import/export
	in->dist_state = 0;
	if (istate->is_softasync)
	  in->dist_state |= CINODE_DIST_SOFTASYNC;
  
	// other state? ***

    // dirty?
	if (istate->dirty) {
	  in->mark_dirty();
	  
	  dout(10) << "logging dirty import " << *in << endl;
	  mds->mdlog->submit_entry(new EInodeUpdate(in),
							   NULL);   // FIXME pay attention to completion?
	}
  } else {
	// this is a directory; i am importing a hashed dir
	assert(in->is_dir());
	assert(dir->is_hashed());

	int auth = in->authority(mds->get_cluster());

	if (in->is_auth()) {
	  assert(in->is_cached_by(from));
	  assert(auth == mds->get_nodeid());
	} else {
	  if (auth == from) {
		// do nothing.  from added us to their cached_by.
	  } else {
		if (had_inode) {
		  dout(7) << "   imported collateral dir " << *in << " auth " << auth << ", had it" << endl;
		} else {
		  dout(7) << "   imported collateral dir " << *in << " auth " << auth << ", discovering it" << endl;

		  // send InodeGetReplica
		  int dauth = dir->dentry_authority(dname, mds->get_cluster());
		  mds->messenger->send_message(new MInodeGetReplica(in->ino()),
									   MSG_ADDR_MDS(dauth), MDS_PORT_CACHE, MDS_PORT_CACHE);
		  
		  // freeze dir, note waiting status
		  if (import_hashed_replicate_waiting.count(dir->inode->ino()) == 0) {
			// first for this dir.. freeze and add to freeze list!
			import_hashed_frozen_waiting.insert(pair<inodeno_t,inodeno_t>(import_root->ino(), dir->inode->ino()));
			dir->freeze_dir(NULL);  // shouldn't hang, since we're newly authoritative.
		  }
		  import_hashed_replicate_waiting.insert(pair<inodeno_t,inodeno_t>(dir->inode->ino(), in->ino()));
		  
		  // add waiter
		  in->add_waiter(CINODE_WAIT_GETREPLICA, 
						 new C_MDS_ImportHashedReplica(mds, in, dir->inode->ino(), in->ino()));
		}
	  }
	}
  }
  
  return in;
}


void MDCache::import_dir_block(pchar& p, 
							   CInode *containing_import, 
							   int oldauth,
							   CInode *import_root)
{
  // set up dir
  Dir_Export_State_t *dstate = (Dir_Export_State_t*)p;
  dout(7) << " import_dir_block " << dstate->ino << " " << dstate->nitems << " items" << endl;
  CInode *idir = get_inode(dstate->ino);
  assert(idir);

  if (!idir->dir) idir->dir = new CDir(idir, mds->get_nodeid());

  idir->dir->version = dstate->version;
  if (idir->dir_is_hashed()) {
	// hashed
	assert(idir->dir->is_hashed());  // i should already know!
  } else {
	// normal
	idir->dir->state = dstate->state & CDIR_MASK_STATE_EXPORTED;  // we only import certain state
  }
  idir->dir->dir_rep = dstate->dir_rep;
  idir->dir->popularity = dstate->popularity;
  
  assert(!idir->dir->is_auth());
  idir->dir->state_set(CDIR_STATE_AUTH);
  
  p += sizeof(*dstate);
  for (int nrep = dstate->ndir_rep_by; nrep > 0; nrep--) {
	idir->dir->dir_rep_by.insert( *((int*)p) );
	p += sizeof(int);
  }

  // take all waiters on this dir
  // NOTE: a pass of imported data is guaranteed to get all of my waiters because
  // a replica's presense in my cache implies/forces it's presense in authority's.
  list<Context*>waiters;
  idir->dir->take_waiting(CDIR_WAIT_ANY, waiters);
  for (list<Context*>::iterator it = waiters.begin();
	   it != waiters.end();
	   it++) 
	import_root->dir->add_waiter(CDIR_WAIT_IMPORTED, *it);


  // contents
  for (long nitems = dstate->nitems; nitems>0; nitems--) {
	CInode *in = import_dentry_inode(idir->dir, p, oldauth, import_root);
	
	// was this an export?
	if (in->dir_auth >= 0) {
	  
	  // to us?
	  if (in->dir_auth == mds->get_nodeid()) {
		// adjust the import
		dout(7) << " importing nested export " << *in << " to ME!  how fortuitous" << endl;
		imports.erase(in);
		in->dir->state_clear(CDIR_STATE_IMPORT);

		mds->logger->inc("immyex");

		// move nested exports under containing_import
		for (pair<multimap<CInode*,CInode*>::iterator, multimap<CInode*,CInode*>::iterator> p =
			   nested_exports.equal_range(in);
			 p.first != p.second;
			 p.first++) {
		  CInode *nested = (*p.first).second;
		  dout(7) << "     moving nested export " << nested << " under " << containing_import << endl;
		  nested_exports.insert(pair<CInode*,CInode*>(containing_import, nested));
		}

		// de-list under old import
		nested_exports.erase(in);	

		in->dir_auth = CDIR_AUTH_PARENT;
		in->put(CINODE_PIN_IMPORT);       // imports are pinned, no longer import
	  } else {
		
		dout(7) << " importing nested export " << *in << " to " << in->dir_auth << endl;
		// add this export
		in->get(CINODE_PIN_EXPORT);           // all exports are pinned
		exports.insert(in);
		nested_exports.insert(pair<CInode*,CInode*>(containing_import, in));
		mds->logger->inc("imex");
	  }

	}
    //} else in->dir_auth = CDIR_AUTH_PARENT;
  }
 
}


void MDCache::got_hashed_replica(CInode *in,
								 inodeno_t dir_ino,
								 inodeno_t replica_ino)
{

  dout(7) << "got_hashed_replica for import " << *in << " ino " << replica_ino << " in dir " << dir_ino << endl;
  
  // remove from import_hashed_replicate_waiting.
  for (multimap<inodeno_t,inodeno_t>::iterator it = import_hashed_replicate_waiting.find(dir_ino);
	   it != import_hashed_replicate_waiting.end();
	   it++) {
	if (it->second == replica_ino) {
	  import_hashed_replicate_waiting.erase(it);
	  break;
	} else 
	  assert(it->first == dir_ino); // it better be here!
  }
  
  // last one for that dir?
  CInode *idir = get_inode(dir_ino);
  assert(idir && idir->dir);
  if (import_hashed_replicate_waiting.count(dir_ino) > 0)
	return;  // still more
  
  // done with this dir!
  idir->dir->unfreeze_dir();
  
  // remove from import_hashed_frozen_waiting
  for (multimap<inodeno_t,inodeno_t>::iterator it = import_hashed_frozen_waiting.find(in->ino());
	   it != import_hashed_frozen_waiting.end();
	   it++) {
	if (it->second == dir_ino) {
	  import_hashed_frozen_waiting.erase(it);
	  break;
	} else 
	  assert(it->first == in->ino()); // it better be here!
  }
  
  // last one for this import?
  if (import_hashed_frozen_waiting.count(in->ino()) == 0) {
	// all done, we can finish import!
	mds->mdcache->handle_export_dir_finish(in);
  }
}





// authority bystander

void MDCache::handle_export_dir_notify(MExportDirNotify *m)
{
  dout(7) << "handle_export_dir_notify on " << m->get_path() << " new_auth " << m->get_new_auth() << endl;
  
  if (mds->is_shut_down() ||
	  root == NULL) {
	if (mds->get_nodeid() != 0) {
	  dout(5) << "i don't even have root; sending to mds0" << endl;
	  mds->messenger->send_message(m,
								   MSG_ADDR_MDS(0), MDS_PORT_CACHE, MDS_PORT_CACHE);
	} else {
	  dout(5) << "wtf, i'm shut down. " << endl;
	  delete m;
	}
	return;
  }

  vector<CInode*> trav;
  int r = path_traverse(m->get_path(), trav, m, MDS_TRAVERSE_FORWARD);  
  if (r > 0) {
	dout(7) << " fwd or delay" << endl;
	return;
  }
  assert(r == 0);  // FIXME: what if ENOENT, etc.?  
  
  CInode *in = trav[ trav.size()-1 ];

  int iauth = in->authority(mds->get_cluster());
  if (iauth != mds->get_nodeid()) {
	// or not!
	dout(7) << " we're not the authority" << endl;
	mds->messenger->send_message(m,
								 MSG_ADDR_MDS(iauth), MDS_PORT_CACHE,
								 MDS_PORT_CACHE);
	return;
  }

  // yay, we're the authority
  dout(7) << "handle_export_dir_notify on " << *in << " new_auth " << m->get_new_auth() << " updated, telling replicas" << endl;

  assert(in->dir_auth != mds->get_nodeid());  // not already mine, or weirdness

  bool wasmine = in->dir_authority(mds->get_cluster()) == mds->get_nodeid();
  in->dir_auth = m->get_new_auth();
  bool ismine  = in->dir_authority(mds->get_cluster()) == mds->get_nodeid();
  assert(wasmine == ismine);

  send_inode_updates(in);
  
  // done
  delete m;
}





// HASHING

/*
 
 interaction of hashing and export/import:

  - dir->is_auth() is completely independent of hashing.  for a hashed dir,
     - all nodes are partially authoritative
     - all nodes dir->is_hashed() == true
     - all nodes dir->inode->dir_is_hashed() == true
     - one node dir->is_auth == true, the rest == false
  - dir_auth for all items in a hashed dir will likely be explicit.

  - export_dir_walk and import_dir_block take care with dir_auth:
     - on export, -1 is changed to mds->get_nodeid()
     - on import, nothing special, actually.

  - hashed dir files aren't included in export
  - hashed dir dirs ARE include in export, but as replicas.  this is important
    because dirs are needed to tie together hierarchy, for auth to know about
    imports/exports, etc.
    - if exporter is auth, adds importer to cached_by
    - if importer is auth, importer will be fine
    - if third party is auth, sends MExportReplicatedHashed to auth
      - auth sends MExportReplicatedHashedAck to importer, who can proceed
        (ie send export ack) when all such messages are received.

  - dir state is preserved
    - COMPLETE and DIRTY aren't transferred
    - new auth should already know the dir is hashed.
  
*/

// HASH on auth

void MDCache::drop_sync_in_dir(CDir *dir)
{
  for (CDir_map_t::iterator it = dir->begin(); it != dir->end(); it++) {
	CInode *in = it->second->inode;
	if (in->is_auth() && 
		in->is_syncbyme()) {
	  dout(7) << "dropping sticky(?) sync on " << *in << endl;
	  sync_release(in);
	}
  }
}


class C_MDS_HashFreeze : public Context {
public:
  MDS *mds;
  CDir *dir;
  C_MDS_HashFreeze(MDS *mds, CDir *dir) {
	this->mds = mds;
	this->dir = dir;
  }
  virtual void finish(int r) {
	mds->mdcache->hash_dir_finish(dir);
  }
};

class C_MDS_HashComplete : public Context {
public:
  MDS *mds;
  CDir *dir;
  C_MDS_HashComplete(MDS *mds, CDir *dir) {
	this->mds = mds;
	this->dir = dir;
  }
  virtual void finish(int r) {
	mds->mdcache->hash_dir_complete(dir);
  }
};

void MDCache::hash_dir(CDir *dir)
{
  assert(!dir->is_hashing());
  assert(!dir->is_hashed());
  assert(dir->is_auth());
  
  if (dir->is_frozen() ||
	  dir->is_freezing()) {
	dout(7) << " can't hash, freezing|frozen." << endl;
	return;
  }
  
  dout(7) << "hash_dir " << *dir->inode << endl;

  // fix state
  dir->state_set(CDIR_STATE_HASHING);
  dir->auth_pin();

  // start freeze
  dir->freeze_dir(new C_MDS_HashFreeze(mds, dir));

  // make complete
  if (!dir->is_complete()) {
	dout(7) << "hash_dir dir " << *dir->inode << " not complete, fetching" << endl;
	mds->mdstore->fetch_dir(dir->inode,
							new C_MDS_HashComplete(mds, dir));
  } else
	hash_dir_complete(dir);

  // drop any sync or lock if sticky
  if (g_conf.mdcache_sticky_sync_normal ||
	  g_conf.mdcache_sticky_sync_softasync) 
	drop_sync_in_dir(dir);
}

void MDCache::hash_dir_complete(CDir *dir)
{
  assert(dir->is_hashing());
  assert(!dir->is_hashed());
  assert(dir->is_auth());

  // mark dirty to pin in cache
  for (CDir_map_t::iterator it = dir->begin(); it != dir->end(); it++) {
	CInode *in = it->second->inode;
	int dentryhashcode = mds->get_cluster()->hash_dentry( dir->inode->ino(), it->first );
	if (dentryhashcode == mds->get_nodeid()) 
	  in->mark_dirty();
  }
  
  hash_dir_finish(dir);
}

void MDCache::hash_dir_finish(CDir *dir)
{
  assert(dir->is_hashing());
  assert(!dir->is_hashed());
  assert(dir->is_auth());
  
  if (!dir->is_frozen_dir()) {
	dout(7) << "hash_dir_finish !frozen yet " << *dir->inode << endl;
	return;
  }
  if (!dir->is_complete()) {
	dout(7) << "hash_dir_finish !complete, waiting still " << *dir->inode << endl;
	return;  
  }

  dout(7) << "hash_dir_finish dir " << *dir->inode << endl;
  
  // get messages to other nodes ready
  vector<MHashDir*> msgs;
  string path;
  dir->inode->make_path(path);
  for (int i=0; i<mds->get_cluster()->get_num_mds(); i++) {
	msgs.push_back(new MHashDir(path));
  }
  
  // divy up contents
  for (CDir_map_t::iterator it = dir->begin(); it != dir->end(); it++) {
	CInode *in = it->second->inode;
	
	int dentryhashcode = mds->get_cluster()->hash_dentry( dir->inode->ino(), it->first );
	if (dentryhashcode == mds->get_nodeid())
	  continue;      // still mine!

	// giving it away.
	in->version++;   // so log entries are ignored, etc.
	
	// mark my children explicitly mine
	if (in->dir_auth == CDIR_AUTH_PARENT)
	  in->dir_auth = mds->get_nodeid();
	
	// add dentry and inode to message
	msgs[dentryhashcode]->dir_rope.append( it->first.c_str(), it->first.length()+1 );
	msgs[dentryhashcode]->dir_rope.append( in->encode_export_state() );
	
	// fix up my state
	if (in->is_dirty()) in->mark_clean();
	in->cached_by_clear();
	
	assert(in->auth == true);
	in->auth = false;

	// there should be no waiters.
  }

  // send them
  for (int i=0; i<mds->get_cluster()->get_num_mds(); i++) {
	mds->messenger->send_message(msgs[i],
								 MSG_ADDR_MDS(i), MDS_PORT_CACHE, MDS_PORT_CACHE);
  }

  // inode state
  dir->inode->inode.isdir = INODE_DIR_HASHED;
  if (dir->inode->is_auth())
	dir->inode->mark_dirty();

  // dir state
  dir->state_set(CDIR_STATE_HASHED);
  dir->state_clear(CDIR_STATE_HASHING);
  dir->mark_dirty();

  // FIXME: log!

  // unfreeze
  dir->unfreeze_dir();
}

/*
hmm, not going to need to do this for now!

void handle_hash_dir_ack(MHashDirAck *m)
{
  CInode *in = 
  
  // done
  delete m;
}
*/

void MDCache::handle_hash_dir(MHashDir *m)
{
  // traverse to node
  vector<CInode*> trav;
  int r = path_traverse(m->get_path(), trav, m, MDS_TRAVERSE_DISCOVER);   
  if (r > 0) return;  // fw or delay

  CInode *idir = trav[trav.size()-1];
  if (!idir->dir) idir->dir = new CDir(idir, mds->get_nodeid());

  dout(7) << "handle_hash_dir dir " << *idir << endl;

  assert(!idir->dir->is_auth());
  assert(!idir->dir->is_hashed());

  // dir state
  idir->dir->state_set(CDIR_STATE_HASHING);

  // assimilate contents
  int oldauth = m->get_source();
  const char *p = m->dir_rope.c_str();
  const char *pend = p + m->dir_rope.length();
  while (p < pend) {
	CInode *in = import_dentry_inode(idir->dir, p, oldauth);
	in->mark_dirty();  // pin in cache
  }

  // dir state
  idir->dir->state_clear(CDIR_STATE_HASHING);
  idir->dir->state_set(CDIR_STATE_HASHED);
 
  // dir is complete
  idir->dir->mark_complete();
  idir->dir->mark_dirty();

  // inode state
  idir->inode.isdir = INODE_DIR_HASHED;
  if (idir->is_auth()) 
	idir->mark_dirty();

  // FIXME: log

  // done.
  delete m;
}




// UNHASHING

class C_MDS_UnhashFreeze : public Context {
public:
  MDS *mds;
  CDir *dir;
  C_MDS_UnhashFreeze(MDS *mds, CDir *dir) {
	this->mds = mds;
	this->dir = dir;
  }
  virtual void finish(int r) {
	mds->mdcache->unhash_dir_finish(dir);
  }
};

class C_MDS_UnhashComplete : public Context {
public:
  MDS *mds;
  CDir *dir;
  C_MDS_UnhashComplete(MDS *mds, CDir *dir) {
	this->mds = mds;
	this->dir = dir;
  }
  virtual void finish(int r) {
	mds->mdcache->unhash_dir_complete(dir);
  }
};


void MDCache::unhash_dir(CDir *dir)
{
  assert(dir->is_hashed());
  assert(!dir->is_unhashing());
  assert(dir->is_auth());
  
  if (dir->is_frozen() ||
	  dir->is_freezing()) {
	dout(7) << " can't un_hash, freezing|frozen." << endl;
	return;
  }
  
  dout(7) << "unhash_dir " << *dir->inode << endl;

  // fix state
  dir->state_set(CDIR_STATE_UNHASHING);

  // freeze
  dir->freeze_dir(new C_MDS_UnhashFreeze(mds, dir));

  // request unhash from other nodes
  string path;
  dir->inode->make_path(path);
  for (int i=0; i<mds->get_cluster()->get_num_mds(); i++) {
	if (i == mds->get_nodeid()) continue;
	mds->messenger->send_message(new MUnhashDir(path),
								 MSG_ADDR_MDS(i), MDS_PORT_CACHE, MDS_PORT_CACHE);
	unhash_waiting.insert(pair<CDir*,int>(dir,i));
  }
  
  // make complete
  if (!dir->is_complete()) {
	dout(7) << "hash_dir dir " << *dir->inode << " not complete, fetching" << endl;
	mds->mdstore->fetch_dir(dir->inode,
							new C_MDS_UnhashComplete(mds, dir));
  } else
	unhash_dir_complete(dir);

  // drop any sync or lock if sticky
  if (g_conf.mdcache_sticky_sync_normal ||
	  g_conf.mdcache_sticky_sync_softasync)
	drop_sync_in_dir(dir);
}

 
void MDCache::unhash_dir_complete(CDir *dir)
{
  // mark all my inodes dirty (to avoid a race)
  for (CDir_map_t::iterator it = dir->begin(); it != dir->end(); it++) {
	CInode *in = it->second->inode;
	int dentryhashcode = mds->get_cluster()->hash_dentry( dir->inode->ino(), it->first );
	if (dentryhashcode == mds->get_nodeid()) 
	  in->mark_dirty();
  }
  
  unhash_dir_finish(dir);
}


void MDCache::unhash_dir_finish(CDir *dir)
{
  if (!dir->is_frozen_dir()) {
	dout(7) << "unhash_dir_finish still waiting for freeze on " << *dir->inode << endl;
	return;
  }
  if (!dir->is_complete()) {
	dout(7) << "unhash_dir_finish still waiting for complete on " << *dir->inode << endl;
	return;
  }
  if (unhash_waiting.count(dir) > 0) {
	dout(7) << "unhash_dir_finish still waiting for all acks on " << *dir->inode << endl;
	return;
  }
  
  dout(7) << "unhash_dir_finish dir " << dir->inode << endl;
  
  // dir state
  dir->state_clear(CDIR_STATE_HASHED);
  dir->state_clear(CDIR_STATE_UNHASHING);
  dir->mark_dirty();
  dir->mark_complete();
  
  // inode state
  dir->inode->inode.isdir = INODE_DIR_NORMAL;
  dir->inode->mark_dirty();

  // unfreeze!
  dir->unfreeze_dir();
}


void MDCache::handle_unhash_dir_ack(MUnhashDirAck *m)
{
  CInode *idir = get_inode(m->get_ino());
  assert(idir && idir->dir);
  assert(idir->dir->is_auth());
  assert(idir->dir->is_hashed());
  assert(idir->dir->is_unhashing());

  dout(7) << "handle_unhash_dir_ack dir " << *idir << endl;
  
  // assimilate contents
  int oldauth = m->get_source();
  const char *p = m->dir_rope.c_str();
  const char *pend = p + m->dir_rope.length();
  while (p < pend) {
	CInode *in = import_dentry_inode(idir->dir, p, oldauth);
	in->mark_dirty();   // pin in cache
  }

  // remove from waiting list
  multimap<CDir*,int>::iterator it = unhash_waiting.find(idir->dir);
  while (it->second != oldauth) {
	it++;
	assert(it->first == idir->dir);
  }
  unhash_waiting.erase(it);

  unhash_dir_finish(idir->dir);  // try to finish

  // done.
  delete m; 
}


// unhash on non-auth

class C_MDS_HandleUnhashFreeze : public Context {
public:
  MDS *mds;
  CDir *dir;
  int auth;
  C_MDS_HandleUnhashFreeze(MDS *mds, CDir *dir, int auth) {
	this->mds = mds;
	this->dir = dir;
	this->auth = auth;
  }
  virtual void finish(int r) {
	mds->mdcache->handle_unhash_dir_finish(dir, auth);
  }
};

class C_MDS_HandleUnhashComplete : public Context {
public:
  MDS *mds;
  CDir *dir;
  int auth;
  C_MDS_HandleUnhashComplete(MDS *mds, CDir *dir, int auth) {
	this->mds = mds;
	this->dir = dir;
	this->auth = auth;
  }
  virtual void finish(int r) {
	mds->mdcache->handle_unhash_dir_complete(dir, auth);
  }
};


void MDCache::handle_unhash_dir(MUnhashDir *m)
{
  // traverse to node
  vector<CInode*> trav;
  int r = path_traverse(m->get_path(), trav, m, MDS_TRAVERSE_DISCOVER);   
  if (r > 0) return;  // fw or delay

  CInode *idir = trav[trav.size()-1];
  if (!idir->dir) idir->dir = new CDir(idir, mds->get_nodeid());
  CDir *dir = idir->dir;

  dout(7) << "handle_unhash_dir " << *idir << endl;
  
  assert(dir->is_hashed());
  
  int auth = m->get_source();

  // fix state
  dir->state_set(CDIR_STATE_UNHASHING);

  // freeze
  dir->freeze_dir(new C_MDS_HandleUnhashFreeze(mds, dir, auth));

  // make complete
  if (!dir->is_complete()) {
	dout(7) << "handle_unhash_dir dir " << *dir->inode << " not complete, fetching" << endl;
	mds->mdstore->fetch_dir(dir->inode,
							new C_MDS_HandleUnhashComplete(mds, dir, auth));
  } else
	handle_unhash_dir_complete(dir, auth);

  // drop any sync or lock if sticky
  if (g_conf.mdcache_sticky_sync_normal ||
	  g_conf.mdcache_sticky_sync_softasync) 
	drop_sync_in_dir(dir);

  // done with message
  delete m;
}


void MDCache::handle_unhash_dir_complete(CDir *dir, int auth)
{
  // mark all my inodes dirty (to avoid a race)
  for (CDir_map_t::iterator it = dir->begin(); it != dir->end(); it++) {
	CInode *in = it->second->inode;
	int dentryhashcode = mds->get_cluster()->hash_dentry( dir->inode->ino(), it->first );
	if (dentryhashcode == mds->get_nodeid()) 
	  in->mark_dirty();
  }
  
  handle_unhash_dir_finish(dir, auth);
}

void MDCache::handle_unhash_dir_finish(CDir *dir, int auth)
{
  assert(dir->is_unhashing());
  assert(dir->is_hashed());

  if (!dir->is_complete()) {
	dout(7) << "still waiting for complete on " << *dir->inode << endl;
	return;
  }
  if (!dir->is_frozen_dir()) {
	dout(7) << "still waiting for frozen_dir on " << *dir->inode << endl;
	return;
  }

  assert(dir->is_frozen_dir());
  assert(dir->is_complete());

  dout(7) << "handle_unhash_dir_finish " << *dir->inode << endl;
  // okay, we are complete and frozen.
  
  // get message to auth ready
  MUnhashDirAck *msg = new MUnhashDirAck(dir->inode->ino());
  
  // include contents
  for (CDir_map_t::iterator it = dir->begin(); it != dir->end(); it++) {
	CInode *in = it->second->inode;
	
	int dentryhashcode = mds->get_cluster()->hash_dentry( dir->inode->ino(), it->first );
	
	if (dentryhashcode != mds->get_nodeid())
	  continue;      // not mine

	// give it away.
	in->version++;   // so log entries are ignored, etc.
	
	// add dentry and inode to message
	msg->dir_rope.append( it->first.c_str(), it->first.length()+1 );
	msg->dir_rope.append( in->encode_export_state() );
	
	if (in->dir_auth == auth)
	  in->dir_auth = CDIR_AUTH_PARENT;

	// fix up my state
	if (in->is_dirty()) in->mark_clean();
	in->cached_by_clear();
	
	assert(in->auth == true);
	in->auth = false;

	// there should be no waiters.
  }

  // send back to auth
  mds->messenger->send_message(msg,
							   MSG_ADDR_MDS(auth), MDS_PORT_CACHE, MDS_PORT_CACHE);

  // inode state
  dir->inode->inode.isdir = INODE_DIR_NORMAL;
  if (dir->inode->is_auth())
	dir->inode->mark_dirty();

  // dir state
  dir->state_clear(CDIR_STATE_HASHED);
  dir->state_clear(CDIR_STATE_UNHASHING);
  dir->mark_clean();  // it's not mine.

  // FIXME log
  
  // unfreeze
  dir->unfreeze_dir();
}









// debug crap


void MDCache::show_imports()
{
  if (imports.size() == 0) {
	dout(7) << "no imports/exports" << endl;
	return;
  }
  dout(7) << "imports/exports:" << endl;

  set<CInode*> ecopy = exports;

  for (set<CInode*>::iterator it = imports.begin();
	   it != imports.end();
	   it++) {
	dout(7) << "  + import " << **it << endl;
	
	for (pair< multimap<CInode*,CInode*>::iterator, multimap<CInode*,CInode*>::iterator > p = 
		   nested_exports.equal_range( *it );
		 p.first != p.second;
		 p.first++) {
	  CInode *exp = (*p.first).second;
	  dout(7) << "      - ex " << *exp << " to " << exp->dir_auth << endl;
	  assert( get_containing_import(exp) == *it );

	  if (ecopy.count(exp) != 1) {
		dout(7) << " nested_export " << *exp << " not in exports" << endl;
		assert(0);
	  }
	  ecopy.erase(exp);
	}
  }

  if (ecopy.size()) {
	for (set<CInode*>::iterator it = ecopy.begin();
		 it != ecopy.end();
		 it++) 
	  dout(7) << " stray item in exports: " << **it << endl;
	assert(ecopy.size() == 0);
  }
  

}


void MDCache::show_cache()
{
  for (inode_map_t::iterator it = inode_map.begin();
	   it != inode_map.end();
	   it++) {
	dout(7) << "cache " << *((*it).second);
	if ((*it).second->ref) 
	  dout2(7) << " pin " << (*it).second->ref_set;
	if ((*it).second->cached_by.size())
	  dout2(7) << " cache_by " << (*it).second->cached_by;
	dout2(7) << endl;
  }
}


// hack
vector<CInode*> MDCache::hack_add_file(string& fn, CInode *in) {
  
  // root?
  if (fn == "/") {
	if (!root) {
	  root = in;
	  add_inode( in );
	  //dout(7) << " added root " << root << endl;
	} else {
	  root->inode.ino = in->inode.ino;  // bleh
	}
	vector<CInode*> trace;
	trace.push_back(root);
	return trace;
  } 


  // file.
  int lastslash = fn.rfind("/");
  string dirpart = fn.substr(0,lastslash);
  string file = fn.substr(lastslash+1);

  //dout(7) << "dirpart '" << dirpart << "' filepart '" << file << "' inode " << in << endl;
  
  CInode *idir = hack_get_file(dirpart);
  assert(idir);

  //dout(7) << " got dir " << idir << endl;

  if (idir->dir == NULL) {
	dout(4) << " making " << dirpart << " into a dir" << endl;
	idir->dir = new CDir(idir, mds->get_nodeid()); 
	idir->inode.isdir = true;
  }
  
  add_inode( in );
  link_inode( idir, file, in );

  // trim
  //trim();

  vector<CInode*> trace;
  trace.push_back(idir);
  trace.push_back(in);
  while (idir->parent) {
	idir = idir->parent->dir->inode;
	trace.insert(trace.begin(),idir);
  }
  return trace;
}

CInode* MDCache::hack_get_file(string& fn) {
  int off = 1;
  CInode *cur = root;
  
  // dirs
  while (off < fn.length()) {
	unsigned int slash = fn.find("/", off);
	if (slash == string::npos) 
	  slash = fn.length();	
	string n = fn.substr(off, slash-off);

	//dout(7) << " looking up '" << n << "' in " << cur << endl;

	if (cur->dir == NULL) {
	  //dout(7) << "   not a directory!" << endl;
	  return NULL;  // this isn't a directory.
	}

	CDentry* den = cur->dir->lookup(n);
	if (den == NULL) return NULL;   // file dne!
	cur = den->inode;
	off = slash+1;	
  }

  //dump();
  lru->lru_status();

  return cur;  
}
