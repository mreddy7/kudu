// Copyright (c) 2012, Cloudera, inc

#include <boost/scoped_ptr.hpp>
#include <boost/utility/binary.hpp>
#include <gtest/gtest.h>
#include <glog/logging.h>
#include <stdlib.h>

#include "cfile.h"
#include "cfile.pb.h"
#include "index_block.h"
#include "util/env.h"

namespace kudu { namespace cfile {

TEST(TestCFile, TestGroupVarInt) {
  std::string buf;
  IntBlockBuilder::AppendGroupVarInt32(
    &buf, 0, 0, 0, 0);
  ASSERT_EQ(5UL, buf.size());
  ASSERT_EQ(0, memcmp("\x00\x00\x00\x00\x00", buf.c_str(), 5));
  buf.clear();

  // All 1-byte
  IntBlockBuilder::AppendGroupVarInt32(
    &buf, 1, 2, 3, 254);
  ASSERT_EQ(5UL, buf.size());
  ASSERT_EQ(0, memcmp("\x00\x01\x02\x03\xfe", buf.c_str(), 5));
  buf.clear();

  // Mixed 1-byte and 2-byte
  IntBlockBuilder::AppendGroupVarInt32(
    &buf, 256, 2, 3, 65535);
  ASSERT_EQ(7UL, buf.size());
  ASSERT_EQ( BOOST_BINARY( 01 00 00 01 ), buf.at(0));
  ASSERT_EQ(256, *reinterpret_cast<uint16_t *>(&buf[1]));
  ASSERT_EQ(2, *reinterpret_cast<uint8_t *>(&buf[3]));
  ASSERT_EQ(3, *reinterpret_cast<uint8_t *>(&buf[4]));
  ASSERT_EQ(65535, *reinterpret_cast<uint16_t *>(&buf[5]));
}

TEST(TestCFile, TestIntBlockEncoder) {
  boost::scoped_ptr<WriterOptions> opts(new WriterOptions());
  IntBlockBuilder ibb(opts.get());
  for (int i = 0; i < 10000; i++) {
    ibb.Add(random());
  }
  Slice s = ibb.Finish();
  LOG(INFO) << "Encoded size for 10k ints: " << s.size();

  // Test empty case -- should be 5 bytes for just the
  // header word (all zeros)
  ibb.Reset();
  s = ibb.Finish();
  ASSERT_EQ(5UL, s.size());
}

// TODO: need to write test for round-trip
// int block encoding once there is a reader

#define ASSERT_STATUS_OK(status) do { \
    Status _s = status; \
    if (_s.ok()) { \
      SUCCEED(); \
    } else { \
      FAIL() << "Bad status: " << _s.ToString();  \
    } \
  } while (0);


// Test IndexBlockBuilder and IndexReader with integers
TEST(TestIndexBuilder, TestIndexWithInts) {

  // Encode an index block.
  WriterOptions opts;
  IndexBlockBuilder<uint32_t> idx(&opts);

  const int EXPECTED_NUM_ENTRIES = 4;

  idx.Add(10, BlockPointer(90010, 64*1024));
  idx.Add(20, BlockPointer(90020, 64*1024));
  idx.Add(30, BlockPointer(90030, 64*1024));
  idx.Add(40, BlockPointer(90040, 64*1024));

  size_t est_size = idx.EstimateEncodedSize();
  Slice s = idx.Finish();

  // Estimated size should be between 75-100%
  // of actual size.
  EXPECT_LT(s.size(), est_size);
  EXPECT_GT(s.size(), est_size * 3 /4);

  // Open the encoded block in a reader.
  IndexBlockReader<uint32_t> reader(s);
  ASSERT_STATUS_OK(reader.Parse());

  // Should have all the entries we inserted.
  ASSERT_EQ(EXPECTED_NUM_ENTRIES, (int)reader.Count());

  // Search for a value prior to first entry
  BlockPointer ptr;
  Status status = reader.Search(0, &ptr);
  EXPECT_TRUE(status.IsNotFound());

  // Search for a value equal to first entry
  status = reader.Search(10, &ptr);
  ASSERT_STATUS_OK(status);
  EXPECT_EQ(90010, (int)ptr.offset());
  EXPECT_EQ(64 * 1024, (int)ptr.size());

  // Search for a value between 1st and 2nd entries.
  // Should return 1st.
  status = reader.Search(15, &ptr);
  ASSERT_STATUS_OK(status);
  EXPECT_EQ(90010, (int)ptr.offset());
  EXPECT_EQ(64 * 1024, (int)ptr.size());

  // Search for a value equal to 2nd
  // Should return 2nd.
  status = reader.Search(20, &ptr);
  ASSERT_STATUS_OK(status);
  EXPECT_EQ(90020, (int)ptr.offset());
  EXPECT_EQ(64 * 1024, (int)ptr.size());

  // Between 2nd and 3rd.
  // Should return 2nd
  status = reader.Search(25, &ptr);
  ASSERT_STATUS_OK(status);
  EXPECT_EQ(90020, (int)ptr.offset());
  EXPECT_EQ(64 * 1024, (int)ptr.size());

  // Equal 3rd
  status = reader.Search(30, &ptr);
  ASSERT_STATUS_OK(status);
  EXPECT_EQ(90030, (int)ptr.offset());
  EXPECT_EQ(64 * 1024, (int)ptr.size());

  // Between 3rd and 4th
  status = reader.Search(35, &ptr);
  ASSERT_STATUS_OK(status);
  EXPECT_EQ(90030, (int)ptr.offset());
  EXPECT_EQ(64 * 1024, (int)ptr.size());

  // Equal 4th (last)
  status = reader.Search(40, &ptr);
  ASSERT_STATUS_OK(status);
  EXPECT_EQ(90040, (int)ptr.offset());
  EXPECT_EQ(64 * 1024, (int)ptr.size());

  // Greater than 4th (last)
  status = reader.Search(45, &ptr);
  ASSERT_STATUS_OK(status);
  EXPECT_EQ(90040, (int)ptr.offset());
  EXPECT_EQ(64 * 1024, (int)ptr.size());

  idx.Reset();
}

class StringSink: public WritableFile {
 public:
  ~StringSink() { }

  const std::string& contents() const { return contents_; }

  virtual Status Close() { return Status::OK(); }
  virtual Status Flush() { return Status::OK(); }
  virtual Status Sync() { return Status::OK(); }

  virtual Status Append(const Slice& data) {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }

 private:
  std::string contents_;
};


TEST(TestCFile, TestWriter) {
  Status s;

  WritableFile *file;
  s = Env::Default()->NewWritableFile("/tmp/cfile", &file);

  shared_ptr<WritableFile> sink(file);
  WriterOptions opts;
  // Use a smaller block size to exercise multi-level
  // indexing.
  opts.block_size = 4096;
  Writer w(opts, sink);

  ASSERT_STATUS_OK(w.Start());

  BTreeMetaPB meta;
  meta.set_identifier("test");

  shared_ptr<TreeBuilder> tree;
  s = w.AddTree(meta, &tree);
  ASSERT_STATUS_OK(s);

  // Append 100M values to the test tree
  for (int i = 0; i < 100000000; i++) {
    Status s = tree->Append(i);
    // Dont use ASSERT because it accumulates all the logs
    // even for successes
    if (!s.ok()) {
      FAIL() << "Failed Append(" << i << ")";
    }
  }

  ASSERT_STATUS_OK(w.Finish());
}


} // namespace cfile
} // namespace kudu

int main(int argc, char **argv) {
  google::InstallFailureSignalHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
