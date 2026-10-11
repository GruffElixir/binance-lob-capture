#include "blc/csv/schema.hpp"

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(Schema, ExactHeaders) {
    EXPECT_EQ(kOrderBookHeader,
              "tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_"
              "size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,"
              "ask_size3,ask_size4");
    EXPECT_EQ(kMarketDataHeader,
              "recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,"
              "payload_json");
    EXPECT_EQ(kSnapshotHeader,
              "recv_tsec,recv_tnsec,venue,shard_id,conn_epoch,symbol,apply_before_row,payload_"
              "json");
}

TEST(Schema, ColumnCounts) {
    EXPECT_EQ(column_count(kOrderBookHeader), 26u);
    EXPECT_EQ(column_count(kMarketDataHeader), 9u);
    EXPECT_EQ(column_count(kSnapshotHeader), 8u);
    EXPECT_EQ(column_count(""), 1u);
}

}  // namespace
}  // namespace blc
