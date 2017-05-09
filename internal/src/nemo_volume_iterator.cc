#include "nemo_volume_iterator.h"
#include <algorithm>
//volume scan

nemo::VolumeIterator::~VolumeIterator(){
  delete Hit;
  delete Lit;  
  delete Sit;    
  delete Kit;
}
nemo::VolumeIterator::VolumeIterator(Nemo * nemo, const std::string  &start,const std::string &end, uint64_t limit, bool use_snapshot):
                    n(nemo),end_(end),limit_(limit),count(0),kvt(0){
  Hit = n->HmetaScan(start,end,limit,use_snapshot);
  Lit = n->LmetaScan(start,end,limit,use_snapshot);
  Sit = n->SmetaScan(start,end,limit,use_snapshot);  
  Kit = n->KScan(start,end,limit,use_snapshot);
  kvt.resize(0);
  Init();
}

inline bool cmp(const nemo::KVT & kvt1, const nemo::KVT & kvt2){
  return kvt1.key > kvt2.key;
}

void nemo::VolumeIterator::Init(){
  valid_ = false;

  if(Hit->Valid())
  {
    kvt.push_back(KVT{Hit->key(),Hit->value().vol,DataType::kHSize});
  }
  if(Lit->Valid())
  {
    kvt.push_back(KVT{Lit->key(),Lit->value().vol,DataType::kLMeta});
  }
  if(Sit->Valid())
  {
    kvt.push_back(KVT{Sit->key(),Sit->value().vol,DataType::kSSize});
  }
  if(Kit->Valid())
  {
    kvt.push_back(KVT{Kit->key(),Kit->value().size()+Kit->key().size(), DataType::kKv});
  }
  make_heap(kvt.begin(),kvt.end(),cmp);
  if(kvt.size()>0)
  {
    if(kvt[0].key <= end_){
      valid_ = true;
      count++;
    }
  }
/*
  for(KVT tmp: kvt){
      std::cout <<"after init" << tmp.key <<" " << tmp.volume <<" "<< tmp.kType << std::endl;
  }
*/
}

void nemo::VolumeIterator::Next(){
  char nextType = 0;
  if(kvt.size()>0){
    nextType = kvt.begin()->kType;
    pop_heap(kvt.begin(),kvt.end(),cmp);
    kvt.pop_back();
  }
  switch(nextType){
    case  DataType::kHSize:
      Hit->Next();
      if(Hit->Valid() && Hit->key()<= end_){
          kvt.push_back(KVT{Hit->key(),Hit->value().vol,DataType::kHSize});
          push_heap(kvt.begin(),kvt.end(),cmp);
      }
      break;
    case  DataType::kLMeta:
      Lit->Next();
      if(Lit->Valid() && Lit->key()<= end_){
          kvt.push_back(KVT{Lit->key(),Lit->value().vol,DataType::kLMeta});
          push_heap(kvt.begin(),kvt.end(),cmp);
      }
      break;
    case  DataType::kSSize:
      Lit->Next();
      if(Lit->Valid() && Lit->key()<= end_){
          kvt.push_back(KVT{Lit->key(),Lit->value().vol,DataType::kSSize});
          push_heap(kvt.begin(),kvt.end(),cmp);
      }
      break;      
    case  DataType::kKv:
      Kit->Next();
        if(Kit->Valid()&& Kit->key()<=end_){
            kvt.push_back(KVT{Kit->key(),Kit->value().size()+Kit->key().size(),DataType::kKv});
            push_heap(kvt.begin(),kvt.end(),cmp);
      }
      break; 
  }
  if(kvt.size()>0)
  {
    count++;
    if(count<=limit_){
      valid_ = true;
    }
  }
  else{
    valid_ = false;    
  }
}

bool nemo::VolumeIterator::Valid(){
    return valid_;
}

std::string nemo::VolumeIterator::key(){
  if(kvt.size()>0)
    return kvt.begin()->key;
  else
    return "";
}

int64_t nemo::VolumeIterator::value(){
  if(kvt.size()>0)
    return kvt.begin()->volume;
  else
    return 0;
}

char nemo::VolumeIterator::type(){
  if(kvt.size()>0)
    return kvt.begin()->kType;
  else
    return 0;
}

nemo::Status nemo::RangeDel(Nemo * nemo,const std::string  & start, const std::string & end, uint64_t limit){
    VolumeIterator * it = new VolumeIterator(nemo,start,end,limit);
    int64_t count;
    nemo::Status s;
    for(;it->Valid();it->Next()){
            s = nemo->Del(it->key(),&count);
            if(!s.ok()){
                delete it;
                return s;
            }
    }
    delete it;
    return nemo::Status::OK();
}

nemo::Status nemo::RangeDelWithHandle(Nemo * nemo, rocksdb::DBWithTTL * db,const std::string  & start, const std::string & end, uint64_t limit){
    KIterator * it = nemo->KScanWithHandle(db,start,end,limit);
    int64_t count;
    nemo::Status s;
    for(;it->Valid();it->Next()){
            s = nemo->KDelWithHandle(db,it->key(),&count);
            if(!s.ok()){
                delete it;
                return s;
            }
    }
    delete it;
    return nemo::Status::OK();
}
