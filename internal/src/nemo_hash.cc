#include "nemo_hash.h"

#include <climits>
#include <ctime>
#include <unistd.h>
#include "nemo.h"
#include "nemo_iterator.h"
#include "nemo_mutex.h"
#include "util.h"
#include "xdebug.h"

using namespace nemo;
Status Nemo::HGetMetaByKey(const std::string& key, HashMeta& meta) {
  std::string meta_val, meta_key = EncodeHsizeKey(key);
  Status s = hash_db_->Get(rocksdb::ReadOptions(), meta_key, &meta_val);
  if (!s.ok()) {
    return s;
  }
  if(!meta.DecodeFrom(meta_val))
    return Status::Corruption("parse hashmeta error"); 
  else
    return Status::OK();
}

Status Nemo::HChecknRecover(const std::string& key) {
  RecordLock l(&mutex_hash_record_, key);
  HashMeta meta;
  Status s = HGetMetaByKey(key, meta);
  if (!s.ok()) {
    return s;
  }
  // Generate prefix
  std::string key_start = EncodeHashKey(key, "");
  // Iterater and cout
  int64_t field_count = 0;
  int64_t volume = 0;
  rocksdb::Iterator *it;
  rocksdb::ReadOptions iterate_options;
  iterate_options.snapshot = hash_db_->GetSnapshot();
  iterate_options.fill_cache = false;
  it = hash_db_->NewIterator(iterate_options);
  it->Seek(key_start);
  std::string dbkey, dbfield;
  while (it->Valid()) {
    if ((it->key())[0] != DataType::kHash) {
      break;
    }
    DecodeHashKey(it->key(), &dbkey, &dbfield);
    if (dbkey != key) {
      break;
    }
    ++field_count;
    volume += dbkey.size() + dbfield.size() + it->value().size();        // add volume statistic
    it->Next();
  }
  hash_db_->ReleaseSnapshot(iterate_options.snapshot);
  delete it;
  
  // Compare
  if (meta.len == field_count && meta.vol == volume ) {
    return Status::OK();
  }
  // Fix if needed
  rocksdb::WriteBatch writebatch;
  if (IncrHSize(key, (field_count - meta.len), (volume - meta.vol), writebatch) == -1) {
    return Status::Corruption("fix hash meta failed");
  }
  return hash_db_->WriteWithOldKeyTTL(w_opts_nolog(), &(writebatch));
}

Status Nemo::HCheckMetaKey(const std::string& key) {
    RecordLock l(&mutex_hash_record_, key);
    HashMeta meta;
    Status s = HGetMetaByKey(key, meta);
    if (!s.ok()) {
      return s;
    }
    // Generate prefix
    std::string key_start = EncodeHashKey(key, "");
    // Iterater and cout
    int64_t field_count = 0;
    int64_t volume = 0;
    rocksdb::Iterator *it;
    rocksdb::ReadOptions iterate_options;
    iterate_options.snapshot = hash_db_->GetSnapshot();
    iterate_options.fill_cache = false;
    it = hash_db_->NewIterator(iterate_options);
    it->Seek(key_start);
    std::string dbkey, dbfield;
    while (it->Valid()) {
      if ((it->key())[0] != DataType::kHash) {
        break;
      }
      DecodeHashKey(it->key(), &dbkey, &dbfield);
      if (dbkey != key) {
        break;
      }
      ++field_count;
      volume += dbkey.size() + dbfield.size() + it->value().size();        // add volume statistic
      it->Next();
    }
    hash_db_->ReleaseSnapshot(iterate_options.snapshot);
    delete it;
    
    // Compare
    if (meta.len == field_count && meta.vol == volume ) {
      return Status::OK();
    } else {
      char err_msg[1024];
      sprintf(err_msg,"meta key[%s]: len[%ld],vol[%ld].Summary calculated from data key: len[%ld],vol[%ld].",
                            key.c_str(),meta.len,meta.vol,field_count,volume);
      return Status::Corruption(err_msg);
    }

}

Status Nemo::HSet(const rocksdb::Slice &key, const rocksdb::Slice &field, const rocksdb::Slice &val, int * res) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;

    //RecordLock l(&mutex_hash_record_, key);
    //MutexLock l(&mutex_hash_);
    rocksdb::WriteBatch writebatch;

    //sleep(8);

    int ret = DoHSet(key, field, val, writebatch);
    if (ret > 0) {
        if (IncrHSize(key, ret, key.size()+field.size() + val.size() , writebatch) == -1) {
            //hash_record_.Unlock(key);
            return Status::Corruption("incrhsize error");
        }
        *res = 1;
    }
    else
        *res = 0;
    s = hash_db_->WriteWithOldKeyTTL(w_opts_nolog(), &(writebatch));

    //hash_record_.Unlock(key);
    return s;
}

Status Nemo::HSetNoLock(const std::string &key, const std::string &field, const std::string &val) {
    Status s;
    rocksdb::WriteBatch writebatch;
    int ret = DoHSet(key, field, val, writebatch);
    if (ret > 0) {
        if (IncrHSize(key, ret, key.size()+field.size() + val.size(), writebatch) == -1) {
            return Status::Corruption("incrhsize error");
        }
    }
    s = hash_db_->WriteWithOldKeyTTL(w_opts_nolog(), &(writebatch));
    return s;
}

Status Nemo::HGet(const rocksdb::Slice &key, const rocksdb::Slice &field, std::string *val) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    std::string dbkey = EncodeHashKey(key, field);
    Status s = hash_db_->Get(rocksdb::ReadOptions(), dbkey, val);
    return s;
}

Status Nemo::HDel(const rocksdb::Slice &key, const rocksdb::Slice &field) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;
    //RecordLock l(&mutex_hash_record_, key);
    rocksdb::WriteBatch writebatch;
    int64_t ret = DoHDel(key, field, writebatch);
    if (ret > 0) {
        if (IncrHSize(key, -1, -key.size()-field.size()-ret, writebatch) == -1) {
            return Status::Corruption("incrlen error");
        }
        s = hash_db_->Write(rocksdb::WriteOptions(), &(writebatch));
        return s;
    } else if (ret == 0) {
        return Status::NotFound(); 
    } else {
        return Status::Corruption("DoHDel error");
    }
}
//add HMDel for redis api
Status Nemo::HMDel(const std::string &key, const std::vector<std::string> &fields, int64_t * res) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;
    RecordLock l(&mutex_hash_record_, key);
    rocksdb::WriteBatch writebatch;
    *res = 0;
    for(std::string field:fields)
    {
        int64_t ret = DoHDel(key, field, writebatch);
        if (ret > 0) {
            if (IncrHSize(key, -1, -key.size()-field.size()-ret, writebatch) == -1){
                return Status::Corruption("incrlen error");
            }
            else{
                (*res)++;
            }
        } else if (ret == 0){
            continue;
        }
        else {
            return Status::Corruption("DoHDel error");
        }
    }
    if(*res>0)
        s = hash_db_->Write(rocksdb::WriteOptions(), &(writebatch));
    return Status::OK();
}

// Note: No lock, Internal use only!!
Status Nemo::HDelKey(const std::string &key, int64_t *res) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;
    std::string val;
    std::string size_key = EncodeHsizeKey(key);
    *res = 0;

    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, &val);
    if (s.ok()) {
      HashMeta meta;
      if(!meta.DecodeFrom(val))
        return Status::Corruption("parse hashmeta error");       
      if (meta.len <= 0) {
        s = Status::NotFound("");
      } else {
        *res = 1;
        meta.len = 0;
        meta.vol = 0;
        meta.EncodeTo(val);
        s = hash_db_->PutWithKeyVersion(rocksdb::WriteOptions(), size_key, val);
      }
    }

    return s;
}

Status Nemo::HExpire(const std::string &key, const int32_t seconds, int64_t *res) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;
    std::string val;

    RecordLock l(&mutex_hash_record_, key);
    std::string size_key = EncodeHsizeKey(key);
    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, &val);
    if (s.IsNotFound()) {
        *res = 0;
    } else if (s.ok()) {
      HashMeta meta;
      if(!meta.DecodeFrom(val))
        return Status::Corruption("parse hashmeta error");       
      if (meta.len <= 0) {
        return Status::NotFound("");
      }
      meta.len = 0;
      meta.vol = 0;
      meta.EncodeTo(val);
      if (seconds > 0) {
        //MutexLock l(&mutex_hash_);
        s = hash_db_->Put(w_opts_nolog(), size_key, val, seconds);
      } else { 
        int64_t count;
        s = HDelKey(key, &count);
      }
      *res = 1;
    }
    return s;
}

Status Nemo::HTTL(const std::string &key, int64_t *res) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;
    std::string val;

    std::string size_key = EncodeHsizeKey(key);
    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, &val);
    if (s.IsNotFound()) {
        *res = -2;
    } else if (s.ok()) {
        int32_t ttl;
        s = hash_db_->GetKeyTTL(rocksdb::ReadOptions(), size_key, &ttl);
        *res = ttl;
    }
    return s;
}

Status Nemo::HExists(const std::string &key, const std::string &field, bool * ifExist) {
    Status s;
    std::string dbkey = EncodeHashKey(key, field);
    std::string val;
    s = hash_db_->Get(rocksdb::ReadOptions(), dbkey, &val);
    if (s.ok()) {
        *ifExist = true;
    } else {
        *ifExist = false;
    }
    return s;
}

Status Nemo::HPersist(const std::string &key, int64_t *res) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;
    std::string val;

    RecordLock l(&mutex_hash_record_, key);

    *res = 0;
    std::string size_key = EncodeHsizeKey(key);
    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, &val);

    if (s.ok()) {
        int32_t ttl;
        s = hash_db_->GetKeyTTL(rocksdb::ReadOptions(), size_key, &ttl);
        if (s.ok() && ttl >= 0) {
            //MutexLock l(&mutex_hash_);
            s = hash_db_->Put(w_opts_nolog(), size_key, val);
            *res = 1;
        }
    }
    return s;
}

Status Nemo::HExpireat(const std::string &key, const int32_t timestamp, int64_t *res) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;
    std::string val;

    //MutexLock l(&mutex_hash_);
    RecordLock l(&mutex_hash_record_, key);

    std::string size_key = EncodeHsizeKey(key);
    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, &val);
    if (s.IsNotFound()) {
        *res = 0;
    } else if (s.ok()) {
      HashMeta meta;
      if(!meta.DecodeFrom(val))
        return Status::Corruption("parse hashmeta error");   
      if (meta.len <= 0) {
        return Status::NotFound("");
      }

      std::time_t cur = std::time(0);
      if (timestamp <= cur) {
        int64_t count;
        s = HDelKey(key, &count);
      } else {
        s = hash_db_->PutWithExpiredTime(w_opts_nolog(), size_key, val, timestamp);
      }
      *res = 1;
    }
    return s;
}

Status Nemo::HKeys(const std::string &key, std::vector<std::string> &fields) {
    std::string dbkey;
    std::string dbfield;
    std::string key_start = EncodeHashKey(key, "");
    rocksdb::Iterator *it;
    rocksdb::ReadOptions iterate_options;
    iterate_options.snapshot = hash_db_->GetSnapshot();
    iterate_options.fill_cache = false;
    it = hash_db_->NewIterator(iterate_options);
    it->Seek(key_start);
    while (it->Valid()) {
       if ((it->key())[0] != DataType::kHash) {
           break;
       }
       DecodeHashKey(it->key(), &dbkey, &dbfield);
       if (dbkey == key) {
           fields.push_back(dbfield);
       } else {
           break;
       }
       it->Next();
    }
    hash_db_->ReleaseSnapshot(iterate_options.snapshot);
    delete it;
    return Status::OK();
}

Status Nemo::HLen(const rocksdb::Slice &key,int64_t * len) {
    HashMeta meta;
    if(HSize(key,meta)){
        *len = meta.len;
        return Status::OK();
    }
    else{
        *len = -1;
        return Status::Corruption("hash meta key corruption");
    }  
}

Status Nemo::HGetall(const std::string &key, std::vector<FV> &fvs) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    std::string dbkey;
    std::string dbfield;
    std::string key_start = EncodeHashKey(key, "");
    rocksdb::Iterator *it;
    rocksdb::ReadOptions iterate_options;
    iterate_options.snapshot = hash_db_->GetSnapshot();
    iterate_options.fill_cache = false;
    it = hash_db_->NewIterator(iterate_options);
    it->Seek(key_start);
    while (it->Valid()) {
       if ((it->key())[0] != DataType::kHash) {
           break;
       }
       DecodeHashKey(it->key(), &dbkey, &dbfield);
       if(dbkey == key) {
           fvs.push_back(FV{dbfield, it->value().ToString()});
       } else {
           break;
       }
       it->Next();
    }
    hash_db_->ReleaseSnapshot(iterate_options.snapshot);
    delete it;
    return Status::OK();
}

Status Nemo::HMSet(const std::string &key, const std::vector<FV> &fvs,int * res_list ) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }
    Status s;
    int res;
    std::vector<FV>::const_iterator it;
    for (it = fvs.begin(); it != fvs.end(); it++,res_list++) {
        HSet(key, it->field, it->val, &res);
        *res_list = res;
    }
    return s;
}

Status Nemo::HMSetSlice(const rocksdb::Slice &key, const std::vector<FVSlice> &fvs, int * res_list) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }
    Status s;
    std::string size_key = EncodeHsizeKey(key);
    HashMeta meta;
    std::string old_meta_val,new_meta_val;

    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, &old_meta_val);
    if (s.IsNotFound()) {
        meta.len = 0;
        meta.vol = 0;
    } else if(!s.ok()) {
        return s;
    } else {
        if (old_meta_val.size() < (sizeof(uint64_t)+ sizeof(uint64_t))) {
            return Status::Corruption("the length of hash meta key is wrong");
        }
        else{
            meta.DecodeFrom(old_meta_val);
        }
    }

    rocksdb::WriteBatch writebatch;
    std::vector<FVSlice>::const_iterator it;
    std::string db_val;
    for (it = fvs.begin(); it != fvs.end(); it++,res_list++) {
         std::string dbkey = EncodeHashKey(key, it->field);
         Status s = hash_db_->Get(rocksdb::ReadOptions(), dbkey, &db_val);
         if (s.IsNotFound()) { // not found
            std::string hkey = EncodeHashKey(key, it->field);
            writebatch.Put(hkey, it->val);
            *res_list = 1;
            meta.len++;
            meta.vol +=  key.size()+it->field.size() + it->val.size();
        } else if (s.ok()) {
            if(db_val != it->val){
                std::string hkey = EncodeHashKey(key, it->field);
                writebatch.Put(hkey, it->val);
                meta.vol += it->val.size() - db_val.size();
                *res_list = 1;
            }
            else {
                *res_list = 0;
            }

        } else {
            return s;
        }
    }

    meta.EncodeTo(new_meta_val);
    writebatch.Put(size_key, new_meta_val);
    s = hash_db_->WriteWithOldKeyTTL(w_opts_nolog(), &(writebatch));
    return s;
}

Status Nemo::HMGet(const std::string &key, const std::vector<std::string> &fields, std::vector<FVS> &fvss) {
    Status s;
    std::vector<std::string>::const_iterator it_key;
    for (it_key = fields.begin(); it_key != fields.end(); it_key++) {
        std::string en_key = EncodeHashKey(key, *(it_key));
        std::string val("");
        s = hash_db_->Get(rocksdb::ReadOptions(), en_key, &val);
        fvss.push_back((FVS){*(it_key), val, s});
    }
    return Status::OK();
}

Status Nemo::HMGetSlice(const rocksdb::Slice &key, const std::vector<rocksdb::Slice> &fields, std::vector<SS> &ss) {
    Status s;
    for (size_t i = 0; i < fields.size(); i++) {
        std::string en_key = EncodeHashKey(key, fields[i]);
        std::string * val = new std::string;
        s = hash_db_->Get(rocksdb::ReadOptions(), en_key, val);
        ss[i] = SS{val,s};
    }
    return Status::OK();
}

HIterator* Nemo::HScan(const std::string &key, const std::string &start, const std::string &end, uint64_t limit, bool use_snapshot) {
    std::string key_start, key_end;
    key_start = EncodeHashKey(key, start);
    if (end.empty()) {
        key_end = "";
    } else {
        key_end = EncodeHashKey(key, end);
    }


    rocksdb::ReadOptions read_options;
    if (use_snapshot) {
        read_options.snapshot = hash_db_->GetSnapshot();
    }
    read_options.fill_cache = false;

    IteratorOptions iter_options(key_end, limit, read_options);
    
    rocksdb::Iterator *it = hash_db_->NewIterator(read_options);
    it->Seek(key_start);
    return new HIterator(it,hash_db_.get() ,iter_options, key); 
}

HmetaIterator * Nemo::HmetaScan( const std::string &start, const std::string &end, uint64_t limit, bool use_snapshot, bool skip_nil_index){
    std::string key_start, key_end;
    key_start = EncodeHsizeKey(start);
    if (end.empty()) {
        key_end = "";
    } else {
        key_end = EncodeHsizeKey(end);
    }

    rocksdb::ReadOptions read_options;
    if (use_snapshot) {
        read_options.snapshot = hash_db_->GetSnapshot();
    }
    read_options.fill_cache = false;

    IteratorOptions iter_options(key_end, limit, read_options);
    
    rocksdb::Iterator *it = hash_db_->NewIterator(read_options);
    it->Seek(key_start);
    return new HmetaIterator(it, hash_db_.get() ,iter_options,start,skip_nil_index); 
}

Status Nemo::HSetnx(const std::string &key, const std::string &field, const std::string &val, int64_t * res) {
    Status s;
    std::string str_val;
    //MutexLock l(&mutex_hash_);
    RecordLock l(&mutex_hash_record_, key);
    s = HGet(key, field, &str_val);
    if (s.IsNotFound()) {
        rocksdb::WriteBatch writebatch;
        int ret = DoHSet(key, field, val, writebatch);
        if (ret > 0) {
            if (IncrHSize(key, ret,key.size()+field.size() + val.size() ,writebatch) == -1) {
                *res = -1;
                return Status::Corruption("incrhsize error");
            }
        }
        s = hash_db_->Write(w_opts_nolog(), &(writebatch));
        *res = 1;
        return s;
    } else if(s.ok()) {
        *res = 0;
//      return Status::Corruption("Already Exist");
        return Status::OK();
    } else {
        *res = 1;        
        return Status::Corruption("HGet Error");
    }
}

Status Nemo::HStrlen(const std::string &key, const std::string &field, int64_t * res_len) {
    Status s;
    std::string val;
    s = HGet(key, field, &val);
    if (s.ok()) {
        *res_len = val.length();
    } else if (s.IsNotFound()) {
        *res_len = 0;
    } else {
        *res_len = -1;
    }
    return s;
}

Status Nemo::HVals(const std::string &key, std::vector<std::string> &vals) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }
    std::string dbkey;
    std::string dbfield;
    std::string key_start = EncodeHashKey(key, "");
    rocksdb::Iterator *it;
    rocksdb::ReadOptions iterate_options;
    iterate_options.snapshot = hash_db_->GetSnapshot();
    iterate_options.fill_cache = false;
    it = hash_db_->NewIterator(iterate_options);
    it->Seek(key_start);
    while (it->Valid()) {
       if ((it->key())[0] != DataType::kHash) {
           break;
       }
       DecodeHashKey(it->key(), &dbkey, &dbfield);
       if (dbkey == key) {
           vals.push_back(it->value().ToString());
       } else {
           break;
       }
       it->Next();
    }
    hash_db_->ReleaseSnapshot(iterate_options.snapshot);
    delete it;
    return Status::OK();
}

Status Nemo::HIncrby(const std::string &key, const std::string &field, int64_t by, std::string &new_val) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }
    Status s;
    std::string val;
    //MutexLock l(&mutex_hash_);
    RecordLock l(&mutex_hash_record_, key);
    s = HGet(key, field, &val);
    if (s.IsNotFound()) {
        new_val = std::to_string(by);
    } else if (s.ok()) {
        int64_t ival;
        if (!StrToInt64(val.data(), val.size(), &ival)) {
            return Status::Corruption("value is not integer");
        } 
        if ((by >= 0 && LLONG_MAX - by < ival) || (by < 0 && LLONG_MIN - by > ival)) {
            return Status::InvalidArgument("Overflow");
        }
        new_val = std::to_string((ival + by));
    } else {
        return Status::Corruption("HIncrby error");
    }
    s = HSetNoLock(key, field, new_val);
    return s;
}

Status Nemo::HIncrbyfloat(const std::string &key, const std::string &field, double by, std::string &new_val) {
    if (key.size() >= KEY_MAX_LENGTH || key.size() <= 0) {
       return Status::InvalidArgument("Invalid key length");
    }

    Status s;
    std::string val;
    std::string res;
    //MutexLock l(&mutex_hash_);
    RecordLock l(&mutex_hash_record_, key);
    s = HGet(key, field, &val);
    if (s.IsNotFound()) {
        res = std::to_string(by);
    } else if (s.ok()) {
        double dval;
        if (!StrToDouble(val.data(), val.size(), &dval)) {
            return Status::Corruption("value is not float");
        }
        
        dval += by;
        if (std::isnan(dval) || std::isinf(dval)) {
            return Status::InvalidArgument("Overflow");
        }
        res  = std::to_string(dval);
    } else {
        return Status::Corruption("HIncrbyfloat error");
    }
    size_t pos = res.find_last_not_of("0", res.size());
    pos = pos == std::string::npos ? pos : pos+1;
    new_val = res.substr(0, pos); 
    if (new_val[new_val.size()-1] == '.') {
        new_val = new_val.substr(0, new_val.size()-1);
    }
    s = HSetNoLock(key, field, new_val);
    return s;
}


int Nemo::DoHSet(const rocksdb::Slice &key, const rocksdb::Slice &field, const rocksdb::Slice val, rocksdb::WriteBatch &writebatch) {
    int ret = 0;
    std::string dbval;
    Status s = HGet(key, field, &dbval);
    if (s.IsNotFound()) { // not found
        std::string hkey = EncodeHashKey(key, field);
        writebatch.Put(hkey, val);
        ret = 1;
    } else {
        if(dbval != val){
            std::string hkey = EncodeHashKey(key, field);
            writebatch.Put(hkey, val);
        }
        ret = 0;
    }
    return ret;
}

int64_t Nemo::DoHDel(const rocksdb::Slice &key, const rocksdb::Slice &field, rocksdb::WriteBatch &writebatch) {
    int64_t ret = 0;
    std::string dbval;
    Status s = HGet(key, field, &dbval);
    if (s.ok()) { 
        std::string hkey = EncodeHashKey(key, field);
        writebatch.Delete(hkey);
        ret = dbval.size();
    } else if(s.IsNotFound()) {
        ret = 0;
    } else {
        ret = -1;
    }
    return ret;
}

Status Nemo::HGetIndexInfo(const rocksdb::Slice &key, std::string ** index){
    std::string size_key = EncodeHsizeKey(key);
    Status s;
    std::string * val = new std::string();
    *index = val;
    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, val);
    if (s.IsNotFound()) {
        return s;
    } else if(!s.ok()) {
        return s;
    } else {
        if (val->size() < sizeof(uint64_t) + sizeof(uint64_t)) {
            return Status::Corruption("the length of hash meta key is wrong");
        }
        else{
            return s;
        }
    }
}

Status Nemo::HSetIndexInfo(const rocksdb::Slice &key, const rocksdb::Slice &index){
    std::string size_key = EncodeHsizeKey(key);
    Status s;
    HashMeta meta;
    std::string old_val,new_val;
    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, &old_val);
    if (s.IsNotFound()) {
        meta.len = 0;
        meta.vol = 0;
    } else if(!s.ok()) {
        return s;
    } else {
        if (old_val.size() < sizeof(uint64_t)+ sizeof(uint64_t)) {
            return Status::Corruption("the length of hash meta key is wrong");
        }
        else {
            meta.DecodeFrom(old_val);
        }
    }
    meta.index = index.data();
    meta.index_len = index.size();
    meta.EncodeTo(new_val);
    rocksdb::WriteBatch writebatch;
    writebatch.Put(size_key,new_val);
    return hash_db_->WriteWithOldKeyTTL(w_opts_nolog(), &(writebatch));

}

bool Nemo::HSize(const rocksdb::Slice &key, HashMeta & meta) {
    std::string size_key = EncodeHsizeKey(key);
    std::string val;
    Status s;

    s = hash_db_->Get(rocksdb::ReadOptions(), size_key, &val);
    if (s.IsNotFound()) {
        meta.len = 0;
        meta.vol = 0;
        return true;
    } else if(!s.ok()) {
        return false;
    } else {
        if (val.size() < (sizeof(uint64_t)+ sizeof(uint64_t))) {
            return false;
        }
        else{
            meta.DecodeFrom(val);
            return true;
        }
    }
}

int Nemo::IncrHSize(const rocksdb::Slice &key, int64_t incrlen ,int64_t incrvol, rocksdb::WriteBatch &writebatch) {
    HashMeta meta;
    if(!HSize(key,meta)){
        return -1;
    }
    meta.len += incrlen;
    meta.vol += incrvol;
    std::string size_key = EncodeHsizeKey(key);
    std::string meta_val;
    meta.EncodeTo(meta_val);
    writebatch.Put(size_key, meta_val);
    return 0;
}

bool HashMeta::DecodeFrom(const std::string &meta_val) {
    if (meta_val.size() < sizeof(int64_t) * 2) {
      return false;
    }
  
    len = *((int64_t *)(meta_val.data()));
    vol = *((int64_t *)(meta_val.data() + sizeof(int64_t)));  
    index_len = meta_val.size() - sizeof(int64_t)*2;
    if(index_len>0)
        index = meta_val.data() + sizeof(int64_t)*2;
    else
        index = nullptr;
    return true;
}

bool HashMeta::EncodeTo(std::string& meta_val) {
    meta_val.clear();
    meta_val.append((char *)&len, sizeof(int64_t));
    meta_val.append((char *)&vol, sizeof(int64_t));
    if(index_len>0)
        meta_val.append(index,index_len);
    return true;
}
