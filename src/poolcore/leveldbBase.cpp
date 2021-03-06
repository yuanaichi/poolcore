#include "poolcore/leveldbBase.h"


using namespace boost::filesystem;

levelDbBase::IteratorType::~IteratorType()
{
  delete iterator;
}


bool levelDbBase::IteratorType::valid()
{
  return iterator && iterator->Valid();
}

void levelDbBase::IteratorType::prev()
{
  if (end) {
    leveldb::ReadOptions options;
    auto lastp = base->getLastPartition();
    if (!lastp.db)
      return;
    id = lastp.id;
    iterator = lastp.db->NewIterator(options);
    iterator->SeekToLast();
  } else if (iterator) {
    iterator->Prev();
  }
  
  while (!iterator->Valid()) {
    if (id.empty())
      return;
    
    cleanup();
    auto p = base->lessPartition(id);
    if (!p.db)
      return;
    
    leveldb::ReadOptions options;
    id = p.id;
    iterator = p.db->NewIterator(options);
    iterator->SeekToLast();
  }
  
  end = false;
}


void levelDbBase::IteratorType::next()
{
  if (iterator)
    iterator->Next();
  while (!iterator->Valid()) {
    if (id.empty())
      return;
   
    cleanup();
    auto p = base->greaterPartition(id);
    if (!p.db)
      return;
    
    leveldb::ReadOptions options;
    id = p.id;
    iterator = p.db->NewIterator(options);
    iterator->SeekToFirst();  
  }
}

void levelDbBase::IteratorType::seekFirst()
{
  auto p = base->getFirstPartition();
  if (id == p.id && iterator) {
    iterator->SeekToFirst();
    return;
  }
  
  cleanup();
  if (!p.db)
    return;
  
  id = p.id;
  leveldb::ReadOptions options;
  iterator = p.db->NewIterator(options);
  iterator->SeekToFirst();  
}

void levelDbBase::IteratorType::seekLast()
{
  auto p = base->getLastPartition();
  if (id == p.id && iterator) {
    iterator->SeekToLast();
    return;  
  }
  
  cleanup();
  if (!p.db)
    return;

  id = p.id;  
  leveldb::ReadOptions options;
  iterator = p.db->NewIterator(options);
  iterator->SeekToLast();
}


RawData levelDbBase::IteratorType::key()
{
  RawData data;
  if (iterator) {
    data.data = (uint8_t*)iterator->key().data();
    data.size = iterator->key().size();
  } else {
    data.data = 0;
    data.size = 0;
  }
  return data;
}

RawData levelDbBase::IteratorType::value()
{
  RawData data;
  if (iterator) {
    data.data = (uint8_t*)iterator->value().data();
    data.size = iterator->value().size();
  } else {
    data.data = 0;
    data.size = 0;
  }
  return data;
}


leveldb::DB *levelDbBase::open(levelDbBase::partition &partition)
{
  if (!partition.db) {
    path partitionPath(_path);
    partitionPath /= partition.id;
    
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::Status status = leveldb::DB::Open(options, partitionPath.c_str(), &partition.db);
  }
  
  return partition.db;
}

levelDbBase::partition levelDbBase::getFirstPartition()
{
  if (!_partitions.empty()) {
    auto &p = _partitions.front();
    open(p);
    return p;
  } else {
    return partition();
  }
}

levelDbBase::partition levelDbBase::getLastPartition()
{
  if (!_partitions.empty()) {
    auto &p = _partitions.back();
    open(p);
    return p;
  } else {
    return partition();
  }
}


leveldb::DB *levelDbBase::getPartition(const std::string &id)
{
  auto It = std::lower_bound(_partitions.begin(),
                             _partitions.end(),
                             id,
                             [](const partition &l, const std::string &r) -> bool { return l.id < r; });
  if (It == _partitions.end() || It->id != id)
    return 0;
  return open(*It);
}

levelDbBase::partition levelDbBase::lessPartition(const std::string &id)
{
  auto It = std::lower_bound(_partitions.begin(),
                             _partitions.end(),
                             id,
                             [](const partition &l, const std::string &r) -> bool { return l.id < r; });
  if (It == _partitions.begin() || --It == _partitions.end())
    return partition();
  
  auto &p = *It;
  open(p);
  return p;
}


levelDbBase::partition levelDbBase::greaterPartition(const std::string &id)
{
  auto It = std::lower_bound(_partitions.begin(),
                             _partitions.end(),
                             id,
                             [](const partition &l, const std::string &r) -> bool { return l.id < r; });
  if (It == _partitions.end() || ++It == _partitions.end())
    return partition();
  
  auto &p = *It;
  open(p);
  return p;
}


levelDbBase::partition levelDbBase::greaterOrEqualPartition(const std::string &id)
{
  auto It = std::lower_bound(_partitions.begin(),
                             _partitions.end(),
                             id,
                             [](const partition &l, const std::string &r) -> bool { return l.id < r; });
  if (It != _partitions.end()) {
    auto &p = *It;
    open(p);
    return p;
  } else {
    return partition();
  }
}

leveldb::DB *levelDbBase::getOrCreatePartition(const std::string &id)
{
  auto It = std::lower_bound(_partitions.begin(),
                             _partitions.end(),
                             id,
                             [](const partition &l, const std::string &r) -> bool { return l.id < r; });
  if (It == _partitions.end() || It->id != id)
    It = _partitions.insert(It, partition(id));
  return open(*It);
}

levelDbBase::levelDbBase(const std::string &path) : _path(path)
{
  create_directories(path);
  
  directory_iterator dirItEnd;
  for (directory_iterator dirIt(path); dirIt != dirItEnd; ++dirIt) {
    if (is_directory(dirIt->status())) {
      // Add a partition
      _partitions.push_back(partition(dirIt->path().filename().c_str()));
      fprintf(stderr, "   * found partition %s for %s\n", dirIt->path().c_str(), path.c_str());
    }
  }
  
  std::sort(_partitions.begin(), _partitions.end());
}

levelDbBase::~levelDbBase()
{
  for (auto &p: _partitions) {
    delete p.db;
    if (p.db)
      fprintf(stderr, "<info> partition %s / %s was closed\n", _path.c_str(), p.id.c_str());
  }
}

bool levelDbBase::put(const std::string &partitionId, const void *key, size_t keySize, const void *value, size_t valueSize)
{
  if (leveldb::DB *db = getOrCreatePartition(partitionId)) {
    leveldb::WriteOptions write_options;
    leveldb::Slice K((const char*)key, keySize);
    leveldb::Slice V((const char*)value, valueSize);
    write_options.sync = true;
    return db->Put(write_options, K, V).ok();
  } else {
    return false;
  }
}

bool levelDbBase::deleteRow(const std::string &partitionId, const void *key, size_t keySize)
{
  if (leveldb::DB *db = getOrCreatePartition(partitionId)) {
    leveldb::WriteOptions write_options;
    leveldb::Slice K((const char*)key, keySize);
    write_options.sync = true;
    return db->Delete(write_options, K).ok();
  } else {
    return false;
  }
}

void levelDbBase::clear()
{
  for (auto I = _partitions.begin(), IE = _partitions.end(); I != IE; ++I) {
    if (leveldb::DB *db = open(*I)) {
      delete db;
      I->db = 0;
    }

    path partitionPath(_path);
    partitionPath /= I->id;
    boost::filesystem::remove_all(partitionPath);
  }
  
  _partitions.clear();
}

levelDbBase::IteratorType *levelDbBase::iterator()
{
  return new IteratorType(this);
}
