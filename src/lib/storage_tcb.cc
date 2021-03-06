/**
 *	storage_tcb.cc
 *
 *	implementation of gree::flare::storage_tcb
 *
 *	@author	Masaki Fujimoto <fujimoto@php.net>
 *
 *	$Id$
 */
#include "app.h"
#include "storage_tcb.h"

namespace gree {
namespace flare {

// {{{ ctor/dtor
/**
 *	ctor for storage_tcb
 */
storage_tcb::storage_tcb(string data_dir, int mutex_slot_size, uint32_t storage_ap, uint64_t storage_bucket_size, int storage_cache_size, string storage_compess, bool storage_large, int storage_lmemb, int storage_nmemb, int32_t storage_dfunit):
		storage(data_dir, mutex_slot_size, storage_cache_size),
		_cursor(NULL) {
	this->_data_path = this->_data_dir + "/flare.bdb";

	this->_db = tcbdbnew();
	tcbdbsetmutex(this->_db);
	compress t = compress_none;
	compress_cast(storage_compess, t);
	int n = 0;
	if (storage_large) {
		n |= HDBTLARGE;
	}
	switch (t) {
	case compress_none:
		break;
	case compress_deflate:
		n |= HDBTDEFLATE;
		break;
	case compress_bz2:
		n |= HDBTBZIP;
		break;
	case compress_tcbs:
		n |= HDBTTCBS;
		break;
	}
	tcbdbtune(this->_db, storage_lmemb, storage_nmemb, storage_bucket_size, storage_ap, 10, n);

	if (storage_dfunit > 0) {
		tcbdbsetdfunit(this->_db, storage_dfunit);
	}
}

/**
 *	dtor for storage
 */
storage_tcb::~storage_tcb() {
	if (this->_open) {
		this->close();
	}
	if (this->_db != NULL) {
		tcbdbdel(this->_db);
	}
}
// }}}

// {{{ operator overloads
// }}}

// {{{ public methods
int storage_tcb::open() {
	if (this->_open) {
		log_warning("storage has been already opened", 0);
		return -1;
	}

	if (tcbdbopen(this->_db, this->_data_path.c_str(), HDBOWRITER | HDBOCREAT) == false) {
		int ecode = tcbdbecode(this->_db);
		log_err("tcbdbopen() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
		return -1;
	}

	log_debug("storage open (path=%s, type=%s)", this->_data_path.c_str(), storage::type_cast(this->_type).c_str());
	this->_open = true;

	return 0;
}

int storage_tcb::close() {
	if (this->_open == false) {
		log_warning("storage is not yet opened", 0);
		return -1;
	}

	if (tcbdbclose(this->_db) == false) {
		int ecode = tcbdbecode(this->_db);
		log_err("tcbdbclose() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
		return -1;
	}

	log_debug("storage close", 0);
	this->_open = false;

	return 0;
}

int storage_tcb::set(entry& e, result& r, int b) {
	log_info("set (key=%s, flag=%d, expire=%ld, size=%llu, version=%llu, behavior=%x)", e.key.c_str(), e.flag, e.expire, e.size, e.version, b);
	int mutex_index = 0;

	if ((b & behavior_skip_lock) == 0) {
		mutex_index = e.get_key_hash_value(hash_algorithm_bitshift) % this->_mutex_slot_size;
	}

	uint8_t* p = NULL;
	try {
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_wrlock(&this->_mutex_slot[mutex_index]);
		}

		entry e_current;
		int e_current_exists = 0;
		if ((b & behavior_append) || (b & behavior_prepend)) {
			result r;
			e_current.key = e.key;
			int n = this->get(e_current, r, behavior_skip_lock);
			if (r == result_not_found || n < 0) {
				this->_get_header_cache(e_current.key, e_current);
				e_current_exists = -1;
			} else {
				e_current_exists = 0;
			}
		} else {
			e_current_exists = this->_get_header(e.key, e_current);
		}

		enum st {
			st_alive,
			st_not_expired,
			st_gone,
		};
		int e_current_st = st_alive;
		if (b != 0) {
			if (e_current_exists == 0) {
				if ((b & behavior_skip_timestamp) == 0 && e_current.expire > 0 && e_current.expire <= stats_object->get_timestamp()) {
					e_current_st = st_gone;
				}
			} else {
				if ((b & behavior_skip_timestamp) == 0 && e_current.expire > 0 && e_current.expire > stats_object->get_timestamp()) {
					e_current_st = st_not_expired;
				} else {
					e_current_st = st_gone;
				}
			}
			log_debug("determined state (st=%d)", e_current_st);
		}
		
		// check for "add"
		if ((b & behavior_add) != 0 && e_current_st != st_gone) {
			log_debug("behavior=add and data exists (or delete queue not expired) -> skip setting", 0);
			r = result_not_stored;
			throw 0;
		}

		// check for "replace"
		if ((b & behavior_replace) != 0 && e_current_st != st_alive) {
			log_debug("behavior=replace and data not found (or delete queue not expired) -> skip setting", 0);
			r = result_not_stored;
			throw 0;
		}

		// check for "cas"
		if ((b & behavior_cas) != 0 && e_current_st == st_gone) {
			log_debug("behavior=cas and data not found -> skip setting", 0);
			r = result_not_found;
			throw 0;
		}

		if (b & behavior_cas) {
			if ((e_current_st != st_not_expired && e.version != e_current.version) || (e_current_st == st_not_expired && e.version != (e_current.version-1))) {
				log_info("behavior=cas and specified version is not equal to current version -> skip setting (current=%llu, specified=%llu)", e_current.version, e.version);
				r = result_exists;
				throw 0;
			}
			e.version++;
		} else if ((b & behavior_skip_version) == 0 && e.version != 0) {
			if ((e_current_exists == 0 || (b & behavior_dump) != 0) && e.version <= e_current.version) {
				log_info("specified version is older than (or equal to) current version -> skip setting (current=%u, specified=%u)", e_current.version, e.version);
				r = result_not_stored;
				throw 0;
			}
		} else if (e.version == 0) {
			e.version = e_current.version+1;
			log_debug("updating version (version=%llu)", e.version);
		}

		if ((b & behavior_append) || (b & behavior_prepend)) {
			if (e_current_st != st_alive) {
				log_warning("behavior=append|prepend but no data exists -> skip setting", 0);
				throw -1;
			}

			// memcached ignores expire in case of append|prepend
			e.expire = e_current.expire;
			p = _new_ uint8_t[entry::header_size + e.size + e_current.size];
			uint64_t e_size = e.size;
			e.size += e_current.size;
			this->_serialize_header(e, p);

			// :(
			if (b & behavior_append) {
				memcpy(p+entry::header_size, e_current.data.get(), e_current.size);
				memcpy(p+entry::header_size+e_current.size, e.data.get(), e_size);
			} else {
				memcpy(p+entry::header_size, e.data.get(), e_size);
				memcpy(p+entry::header_size+e_size, e_current.data.get(), e_current.size);
			}
			shared_byte data(new uint8_t[e.size]);
			memcpy(data.get(), p+entry::header_size, e.size);
			e.data = data;
		} else {
			p = _new_ uint8_t[entry::header_size + e.size];
			this->_serialize_header(e, p);
			memcpy(p+entry::header_size, e.data.get(), e.size);
		}

		if (e.option & option_noreply) {
			// no async for tcb
			if (tcbdbput(this->_db, e.key.c_str(), e.key.size(), p, entry::header_size + e.size) == true) {
				log_debug("stored data [async] (key=%s, size=%llu)", e.key.c_str(), entry::header_size + e.size);
				r = result_stored;
			} else {
				int ecode = tcbdbecode(this->_db);
				log_err("tcbdbput() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
				throw -1;
			}
		} else {
			if (tcbdbput(this->_db, e.key.c_str(), e.key.size(), p, entry::header_size + e.size) == true) {
				log_debug("stored data [sync] (key=%s, size=%llu)", e.key.c_str(), entry::header_size + e.size);
				r = result_stored;
			} else {
				int ecode = tcbdbecode(this->_db);
				log_err("tcbdbput() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
				throw -1;
			}
		}
	} catch (int error) {
		if (p) {
			_delete_(p);
		}
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
		}
		return error;
	}
	if (p) {
		_delete_(p);
	}
	if ((b & behavior_skip_lock) == 0) {
		pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
	}

	return 0;
}

int storage_tcb::incr(entry& e, uint64_t value, result& r, bool increment, int b) {
	log_info("incr (key=%s, value=%llu, increment=%d", e.key.c_str(), value, increment);
	int mutex_index = 0;

	if ((b & behavior_skip_lock) == 0) {
		mutex_index = e.get_key_hash_value(hash_algorithm_bitshift) % this->_mutex_slot_size;
	}

	uint8_t* tmp_data = NULL;
	int tmp_len = 0;
	uint8_t* p = NULL;
	entry e_current;
	bool remove_request = false;
	try {
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_wrlock(&this->_mutex_slot[mutex_index]);
		}

		// get current data (and check existence)
		tmp_data = (uint8_t*)tcbdbget(this->_db, e.key.c_str(), e.key.size(), &tmp_len);
		if (tmp_data == NULL) {
			r = result_not_found;
			throw 0;
		}
		int offset = this->_unserialize_header(tmp_data, tmp_len, e_current);
		if (offset < 0) {
			log_err("failed to unserialize header (data is corrupted) (tmp_len=%d)", tmp_len);
			throw -1;
		}
		if (static_cast<uint64_t>(tmp_len - offset) != e_current.size) {
			log_err("actual data size is different from header data size (actual=%d, header=%u)", tmp_len-offset, e_current.size);
			throw -1;
		}
		if ((b & behavior_skip_timestamp) == 0) {
			if (e_current.expire > 0 && e_current.expire <= stats_object->get_timestamp()) {
				log_info("data expired [expire=%d] -> remove requesting", e_current.expire);
				r = result_not_found;
				remove_request = true;
				throw 0;
			}
		}

		// increment (or decrement)
		uint8_t* q = tmp_data+offset;
		while (*q) {
			if (isdigit(*q) == false) {
				*q = '\0';
				break;
			}
			q++;
		}

		uint64_t n = 0, m;
		try {
			n = lexical_cast<uint64_t>(tmp_data+offset);
		} catch (bad_lexical_cast e) {
			n = 0;
		}
		free(tmp_data);
		tmp_data = NULL;
		m = n + (increment ? value : -value);
		if (increment && m < n) {
			m = 0; m--;
		} else if (increment == false && m > n) {
			m = 0;
		}

		// data
		char buf[64];
		int buf_len = snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(m));

		e.flag = e_current.flag;
		e.expire = e_current.expire;
		e.size = buf_len;
		e.version = e_current.version+1;
		log_debug("updating version (version=%llu)", e.version);
		shared_byte data(new uint8_t[e.size]);
		memcpy(data.get(), buf, buf_len);
		e.data = data;

		// store
		p = _new_ uint8_t[entry::header_size + e.size];
		this->_serialize_header(e, p);
		memcpy(p+entry::header_size, e.data.get(), e.size);

		if (e.option & option_noreply) {
			// no async for tcb
			if (tcbdbput(this->_db, e.key.c_str(), e.key.size(), p, entry::header_size + e.size) == true) {
				log_debug("stored data [async] (key=%s, size=%llu)", e.key.c_str(), entry::header_size + e.size);
				r = result_stored;
			} else {
				int ecode = tcbdbecode(this->_db);
				log_err("tcbdbputasync() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
				throw -1;
			}
		} else {
			if (tcbdbput(this->_db, e.key.c_str(), e.key.size(), p, entry::header_size + e.size) == true) {
				log_debug("stored data [sync] (key=%s, size=%llu)", e.key.c_str(), entry::header_size + e.size);
				r = result_stored;
			} else {
				int ecode = tcbdbecode(this->_db);
				log_err("tcbdbput() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
				throw -1;
			}
		}
	} catch (int error) {
		if (tmp_data != NULL) {
			free(tmp_data);
		}
		if (p) {
			_delete_(p);
		}
		if (remove_request) {
			result r_remove;
			this->remove(e_current, r_remove, behavior_version_equal);		// do not care about result here
		}
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
		}
		return error;
	}
	if (p) {
		_delete_(p);
	}
	if ((b & behavior_skip_lock) == 0) {
		pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
	}

	return 0;
}

int storage_tcb::get(entry& e, result& r, int b) {
	log_info("get (key=%s)", e.key.c_str());
	int mutex_index = 0;
	uint8_t* tmp_data = NULL;
	int tmp_len = 0;
	bool remove_request = false;

	if ((b & behavior_skip_lock) == 0) {
		mutex_index = e.get_key_hash_value(hash_algorithm_bitshift) % this->_mutex_slot_size;
	}

	try {
		if ((b & behavior_skip_lock) == 0) {
			log_debug("getting read lock (index=%d)", mutex_index);
			pthread_rwlock_rdlock(&this->_mutex_slot[mutex_index]);
		}

		tmp_data = (uint8_t*)tcbdbget(this->_db, e.key.c_str(), e.key.size(), &tmp_len);
		if (tmp_data == NULL) {
			r = result_not_found;
			throw 0;
		}

		int offset = this->_unserialize_header(tmp_data, tmp_len, e);
		if (offset < 0) {
			log_err("failed to unserialize header (data is corrupted) (tmp_len=%d)", tmp_len);
			throw -1;
		}
		if (static_cast<uint64_t>(tmp_len - offset) != e.size) {
			log_err("actual data size is different from header data size (actual=%d, header=%u)", tmp_len-offset, e.size);
			throw -1;
		}

		if ((b & behavior_skip_timestamp) == 0) {
			if (e.expire > 0 && e.expire <= stats_object->get_timestamp()) {
				log_info("data expired [expire=%d] -> remove requesting", e.expire);
				r = result_not_found;
				remove_request = true;
				throw 0;
			}
		}

		shared_byte p(new uint8_t[e.size]);
		memcpy(p.get(), tmp_data + offset, tmp_len - offset);
		free(tmp_data);
		e.data = p;
	} catch (int error) {
		if (tmp_data != NULL) {
			free(tmp_data);
		}
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
		}

		if (remove_request) {
			result r_remove;
			this->remove(e, r_remove, behavior_version_equal);		// do not care about result here
		}

		return error;
	}
	if ((b & behavior_skip_lock) == 0) {
		pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
	}
	r = result_none;

	return 0;
}

int storage_tcb::remove(entry& e, result& r, int b) {
	log_info("remove (key=%s, expire=%ld, version=%llu)", e.key.c_str(), e.expire, e.version);
	int mutex_index = 0;

	if ((b & behavior_skip_lock) == 0) {
		mutex_index = e.get_key_hash_value(hash_algorithm_bitshift) % this->_mutex_slot_size;
	}

	try {
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_wrlock(&this->_mutex_slot[mutex_index]);
		}

		entry e_current;
		int e_current_exists = this->_get_header(e.key, e_current);
		if ((b & behavior_skip_version) == 0 && e.version != 0) {
			if (((b & behavior_version_equal) == 0 && e.version <= e_current.version) || ((b & behavior_version_equal) != 0 && e.version != e_current.version)) {
				log_info("specified version is older than (or equal to) current version -> skip removing (current=%u, specified=%u)", e_current.version, e.version);
				r = result_not_found;
				throw 0;
			}
		}

		if (e_current_exists < 0) {
			log_debug("data not found in database -> skip removing and updating header cache if we need", 0);
			if (e.version != 0) {
				this->_set_header_cache(e.key, e);
			}
			r = result_not_found;
			throw 0;
		}

		bool expired = false;
		if ((b & behavior_skip_timestamp) == 0 && e_current.expire > 0 && e_current.expire <= stats_object->get_timestamp()) {
			log_info("data expired [expire=%d] -> result is NOT_FOUND but continue processing", e_current.expire);
			expired = true;
		}

		if (tcbdbout(this->_db, e.key.c_str(), e.key.size()) == true) {
			r = expired ? result_not_found : result_deleted;
			log_debug("removed data (key=%s)", e.key.c_str());
		} else {
			int ecode = tcbdbecode(this->_db);
			log_err("tcbdbout() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
			throw -1;
		}
		if (expired == false) {
			if (e.version == 0) {
				e.version = e_current.version+1;
				log_debug("updating version (version=%llu)", e.version);
			}
			this->_set_header_cache(e.key, e);
		}
	} catch (int error) {
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
		}
		return error;
	}
	if ((b & behavior_skip_lock) == 0) {
		pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
	}

	return 0;
}

int storage_tcb::truncate(int b) {
	log_info("truncating all data", 0);

	int i;
	if ((b & behavior_skip_lock) == 0) {
		for (i = 0; i < this->_mutex_slot_size; i++) {
			pthread_rwlock_wrlock(&this->_mutex_slot[i]);
		}
	}

	int r = 0;
	if (tcbdbvanish(this->_db) == false) {
		int ecode = tcbdbecode(this->_db);
		log_err("tcbdbvanish() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
		r = -1;
	}

	this->_clear_header_cache();

	if ((b & behavior_skip_lock) == 0) {
		for (i = 0; i < this->_mutex_slot_size; i++) {
			pthread_rwlock_unlock(&this->_mutex_slot[i]);
		}
	}

	return r;
}

int storage_tcb::iter_begin() {
	if (this->_cursor != NULL) {
		return -1;
	}

	this->_cursor = tcbdbcurnew(this->_db);
	if (tcbdbcurfirst(this->_cursor) == false) {
		int ecode = tcbdbecode(this->_db);

		if (ecode == TCENOREC) {
			tcbdbcurdel(this->_cursor);
			this->_cursor = NULL;
			return 0;
		}

		log_err("tcbdbcurfirst() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
		tcbdbcurdel(this->_cursor);
		this->_cursor = NULL;
		return -1;
	}

	return 0;
}

storage::iteration storage_tcb::iter_next(string& key) {
	if (this->_cursor == NULL) {
		return iteration_end;
	}

	int len = 0;
	char* p = static_cast<char*>(tcbdbcurkey(this->_cursor, &len));
	if (p == NULL) {
		int ecode = tcbdbecode(this->_db);
		if (ecode == TCESUCCESS || ecode == TCENOREC) {
			// end of iteration
			return iteration_end;
		} else {
			log_err("tcbdbcurkey() failed: %s (%d)", tcbdberrmsg(ecode), ecode);
			return iteration_error;
		}
	}
	key = p;
	free(p);

	tcbdbcurnext(this->_cursor);

	return iteration_continue;
}

int storage_tcb::iter_end() {
	if (this->_cursor == NULL) {
		return 0;
	}
	tcbdbcurdel(this->_cursor);
	this->_cursor = NULL;

	return 0;
}

uint32_t storage_tcb::count() {
	return tcbdbrnum(this->_db);
}

uint64_t storage_tcb::size() {
	return tcbdbfsiz(this->_db);
}

int storage_tcb::get_key(string key, int limit, vector<string>& r) {
	TCLIST* key_list = tcbdbfwmkeys(this->_db, key.c_str(), key.size(), limit);

	int i;
	for (i = 0; i < tclistnum(key_list); i++) {
		int n;
		const char* p = static_cast<const char*>(tclistval(key_list, i, &n));
		if (p == NULL) {
			break;
		}
		string tmp_key = p;
		r.push_back(tmp_key);
	}

	tclistdel(key_list);

	return 0;
}

bool storage_tcb::is_capable(capability c) {
	switch (c) {
	case capability_prefix_search:
		return true;
	case capability_list:
		return true;
	default:
		// nop
		break;
	}

	return false;
}
// }}}

// {{{ protected methods
int storage_tcb::_get_header(string key, entry& e) {
	log_debug("get header from database (key=%s)", key.c_str());
	int tmp_len = entry::header_size;
	const uint8_t* tmp_data = static_cast<const uint8_t*>(tcbdbget3(this->_db, key.c_str(), key.size(), &tmp_len));
	if (tmp_data == NULL) {
		// if not found, check data version cache
		log_debug("header not found in database -> checking cache", 0);
		this->_get_header_cache(key, e);
		return -1;
	}

	int offset = this->_unserialize_header(tmp_data, tmp_len, e);
	if (offset < 0) {
		log_err("failed to unserialize header (data is corrupted) (tmp_len=%d)", tmp_len);
		return -1;
	}

	log_debug("current header from database (key=%s, flag=%u, version=%llu, expire=%ld, size=%llu)", key.c_str(), e.flag, e.version, e.expire, e.size);

	return 0;
}
// }}}

// {{{ private methods
// }}}

}	// namespace flare
}	// namespace gree

// vim: foldmethod=marker tabstop=2 shiftwidth=2 autoindent
