//
// Created by 吴加禹 on 2019-07-18.
//

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

#include "rocksdb/env.h"
#include "util/coding.h"
#include "util/file_reader_writer.h"
#include "util/gflags_compat.h"
#include "util/random.h"
#include "util/stop_watch.h"
#include "unistd.h"
#include "fcntl.h"

#include "blob_file_builder.h"
#include "blob_file_reader.h"
#include "blob_format.h"
#include "env/io_posix.h"
#include "unordered_set"

using GFLAGS_NAMESPACE::ParseCommandLineFlags;

DEFINE_string(dir, "/tmp/hashkv_proto", "");
DEFINE_uint64(num_keys, 10000000, "");
DEFINE_uint64(scan_length, 100000, "");
DEFINE_uint64(scan_times, 10, "");
DEFINE_double(ordered_keys_ratio, 1, "");
DEFINE_uint64(value_size, 1024, "");
DEFINE_bool(direct_write, false, "");
DEFINE_bool(direct_read, false, "");
DEFINE_bool(prefetch_os_buffer, false, "");
DEFINE_bool(prefetch, false, "");
DEFINE_uint64(prefetch_size, 2 * 1024 * 1024, "");
DEFINE_bool(cleanup, true, "");
DEFINE_bool(readahead,
            false, "");

using namespace rocksdb;
using namespace rocksdb::titandb;

Slice GenerateKey(uint64_t id, std::string *dst) {
  PutFixed64(dst, id);
  return Slice(*dst);
}

Slice GenerateValue(Random64 &rnd, std::string *dst) {
  dst->resize(FLAGS_value_size);
  for (uint64_t i = 0; i < FLAGS_value_size; i++) {
    (*dst)[i] = static_cast<char>(' ' + rnd.Uniform(95));
  }
  return Slice(*dst);
}

Status GenerateFile(
    Env *env, std::string name, Random64 &rnd,
    std::vector<uint64_t> &keys, uint64_t begin, uint64_t end,
    std::vector<BlobHandle> *index,
    std::unique_ptr<RandomAccessFileReader> *file_reader) {
  EnvOptions env_options_write;
  if (FLAGS_direct_write) {
    env_options_write.use_direct_writes = true;
  }
  std::string file_name = FLAGS_dir + "/" + name;
  std::unique_ptr<WritableFile> write_file;
  Status s = env->NewWritableFile(file_name, &write_file, env_options_write);
  if (!s.ok()) {
    return s;
  }
  std::unique_ptr<WritableFileWriter> file_writer(
      new WritableFileWriter(std::move(write_file), file_name, env_options_write));
  TitanDBOptions db_options;
  TitanCFOptions cf_options;
  std::unique_ptr<BlobFileBuilder> file_builder(
      new BlobFileBuilder(db_options, cf_options, file_writer.get()));
  for (uint64_t i = begin; i < end; i++) {
    std::string key_str;
    std::string value_str;
    BlobRecord record;
    record.key = GenerateKey(keys[i], &key_str);
    record.value = GenerateValue(rnd, &value_str);
    BlobHandle handle;
    file_builder->Add(record, &handle);
    if (!file_builder->status().ok()) {
      return file_builder->status();
    }
    (*index)[keys[i]] = handle;
  }
  s = file_builder->Finish();
  if (!s.ok()) {
    return s;
  }
  s = file_writer->Sync(true/*use_fsync*/);
  if (!s.ok()) {
    return s;
  }
  s = file_writer->Close();
  if (!s.ok()) {
    return s;
  }

  uint64_t file_size = 0;
  s = env->GetFileSize(file_name, &file_size);
  if (!s.ok()) {
    return s;
  }
  EnvOptions env_options_read;
  if (FLAGS_direct_read) {
    env_options_read.use_direct_reads = true;
  }
  std::unique_ptr<RandomAccessFile> read_file;
  s = env->NewRandomAccessFile(file_name, &read_file, env_options_read);
  if (!s.ok()) {
    return s;
  }
  file_reader->reset(new RandomAccessFileReader(std::move(read_file), file_name));
  return s;
}

void DropPagecache() {
  int pc = open("/proc/sys/vm/drop_caches", O_WRONLY);
  int n = write(pc, "3", 1);
  if (n != 1) {
    printf("drop page cache error: %d, need sudo\n", n);
  }
  close(pc);
  sync();

  sleep(5);
}

class MyRandomAccessFile : public PosixRandomAccessFile {
public:
  int GetFd() {
    return fd_;
  }
};

int main(int argc, char **argv) {
  ParseCommandLineFlags(&argc, &argv, true);

  // Generate keys.
  uint64_t num_keys = FLAGS_num_keys;
  uint64_t num_ordered_keys =
      static_cast<uint64_t>(num_keys * FLAGS_ordered_keys_ratio + 0.5);
  std::vector<uint64_t> keys(num_keys);
  for (uint64_t i = 0; i < num_keys; i++) {
    keys[i] = i;
  }
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::shuffle(keys.begin(), keys.end(), std::default_random_engine(seed));
  std::sort(keys.begin(), keys.begin() + num_ordered_keys);
  std::vector<bool> is_ordered(num_keys);
  for (uint64_t i = 0; i < num_keys; i++) {
    is_ordered[keys[i]] = (i < num_ordered_keys);
  }

  // Generate test files.
  std::vector<BlobHandle> index(num_keys);
  Env *env = Env::Default();
  Status s = env->CreateDirIfMissing(FLAGS_dir);
  if (!s.ok()) {
    printf("Create directory error: %s\n", s.ToString().c_str());
    return 0;
  }
  Random64 rnd(static_cast<uint64_t>(seed));
  std::unique_ptr<RandomAccessFileReader> ordered_reader;
  s = GenerateFile(
      env, "ordered", rnd, keys, 0, num_ordered_keys, &index, &ordered_reader);
  if (!s.ok()) {
    printf("Failed to generate ordered blob file: %s\n", s.ToString().c_str());
    return 0;
  }
  std::unique_ptr<RandomAccessFileReader> unordered_reader;
  s = GenerateFile(
      env, "unordered", rnd, keys, num_ordered_keys, num_keys, &index, &unordered_reader);
  if (!s.ok()) {
    printf("Failed to generate unordered blob file: %s\n", s.ToString().c_str());
    return 0;
  }

  std::unordered_set<uint64_t> prefetched;
  int fd = reinterpret_cast<MyRandomAccessFile *>(unordered_reader->file())->GetFd();

  char buffer[static_cast<size_t>(FLAGS_value_size + 100)];
  char prefetch_buffer[static_cast<size_t>(FLAGS_prefetch_size)];
  size_t prefetch_offset = 0;
  size_t prefetch_size = 0;
  DropPagecache();
  uint64_t time_used = 0;
  std::thread t;
  for (uint64_t k = 0; k < FLAGS_scan_times; k++) {
    uint64_t start_time = env->NowMicros();
    uint64_t start = rnd.Uniform(num_keys - FLAGS_scan_length), j=start;
    if (FLAGS_readahead) {
      t = std::thread([&]{
        for (; j < start + FLAGS_scan_length; j++) {
          if (!is_ordered[j]) {
            uint64_t blockStart = index[j].offset / 4096;
            if (prefetched.find(blockStart) == prefetched.end()) {
              readahead(fd, index[j].offset, index[j].size);
              prefetched.insert(blockStart);
            }
          }
        }
      });
      /*
      for (; j < start + FLAGS_scan_length; j++) {
        int reader = is_ordered[j] ? 0 : 1;
        if (!is_ordered[j]) {
          uint64_t blockStart = index[j].offset / 4096;
          if (prefetched[reader].find(blockStart) == prefetched[reader].end()) {
            readahead(fd[reader], index[j].offset, index[j].size);
            prefetched[reader].insert(blockStart);
          }
        }
      }
      */
    }

    for (uint64_t i = start; i < start + FLAGS_scan_length; i++) {
      BlobHandle handle = index[i];
      BlobRecord record;
      Slice blob;
      OwnedSlice owned_buffer;
      if (!is_ordered[i]) {
        s = unordered_reader->Read(handle.offset, handle.size, &blob, buffer);
        if (s.ok()) {
        }
      } else {
        if (!FLAGS_prefetch) {
          if (FLAGS_prefetch_os_buffer) {
            if (handle.offset + handle.size > prefetch_offset + prefetch_size) {
              s = ordered_reader->file()->Prefetch(handle.offset, FLAGS_prefetch_size);
              prefetch_offset = handle.offset;
              prefetch_size = FLAGS_prefetch_size;
            }
          }
          s = ordered_reader->Read(handle.offset, handle.size, &blob, buffer);
        } else {
          if (handle.offset + handle.size > prefetch_offset + prefetch_size) {
            Slice prefetch_content;
            s = ordered_reader->Read(handle.offset, FLAGS_prefetch_size, &prefetch_content, prefetch_buffer);
            prefetch_offset = handle.offset;
            prefetch_size = prefetch_content.size();
          }
          assert(prefetch_offset <= handle.offset);
          assert(prefetch_offset + prefetch_size >= handle.offset + handle.size);
          blob = Slice(prefetch_buffer + handle.offset - prefetch_offset, handle.size);
        }
      }
      if (!s.ok()) {
        printf("failed to read key %lu, %s\n", i, s.ToString().c_str());
        return 0;
      }
      /*
      if (FLAGS_readahead) {
        int reader = is_ordered[j]?0:1;
        uint64_t blockStart = index[j].offset / 4096;
        if (prefetched[reader].find(blockStart) == prefetched[reader].end()) {
          readahead(fd[reader], index[j].offset, index[j].size);
          prefetched[reader].insert(blockStart);
        }
        j++;
      }
       */

      BlobDecoder decoder;
      s = decoder.DecodeHeader(&blob);
      if (s.ok()) {
        s = decoder.DecodeRecord(&blob, &record, &owned_buffer);
      }
      if (!s.ok()) {
        printf("failed to decode key %lu, %s\n", i, s.ToString().c_str());
        return 0;
      }
      std::string key_str;
      Slice key = GenerateKey(i, &key_str);
      if (key != record.key) {
        printf("key mismatch at %lu\n", i);
        return 0;
      }
    }
    time_used += env->NowMicros() - start_time;
    t.join();
  }
  double throughput = (double)(FLAGS_value_size*FLAGS_scan_length*FLAGS_scan_times)/time_used;
  printf("Elapsed time (us): %lu, throughput: %f MB/s\n", time_used, throughput);
  ordered_reader.reset();
  unordered_reader.reset();
  if (FLAGS_cleanup) {
    s = env->DeleteFile(FLAGS_dir + "/ordered");
    if (!s.ok()) {
      printf("failed to delete ordered file, %s\n", s.ToString().c_str());
      return 0;
    }
    s = env->DeleteFile(FLAGS_dir + "/unordered");
    if (!s.ok()) {
      printf("failed to delete unordered file, %s\n", s.ToString().c_str());
      return 0;
    }
  }
  return 0;
  std::cout << IOError("", "", 0).ToString() << std::endl;   // for pass compilation;
}