#include "blc/parse/envelope.hpp"
#include "blc/parse/json_workspace.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(Envelope, FastPath) {
    EnvelopeSplitter s;
    Envelope e;
    ASSERT_TRUE(s.split(R"({"stream":"btcusdt@trade","data":{"e":"trade","p":"1"}})", e));
    EXPECT_EQ(e.stream, "btcusdt@trade");
    EXPECT_EQ(e.data, R"({"e":"trade","p":"1"})");
    EXPECT_EQ(s.fallbacks(), 0u);
}

TEST(Envelope, ReversedFieldsUseFallback) {
    EnvelopeSplitter s;
    Envelope e;
    ASSERT_TRUE(s.split(R"({"data":{"x":1},"stream":"btcusdt@trade"})", e));
    EXPECT_EQ(e.stream, "btcusdt@trade");
    EXPECT_EQ(e.data, R"({"x":1})");
    EXPECT_EQ(s.fallbacks(), 1u);
}

TEST(Envelope, Malformed) {
    EnvelopeSplitter s;
    Envelope e;
    for (const char* f :
         {"", "{}", R"({"stream":"a"})", R"({"stream":1,"data":{}})",
          R"({"stream":"a","data":[1]})", R"({"stream":"a","stream":"b","data":{}})",
          R"({"stream":"a","data":{})", "not json", R"([{"stream":"a","data":{}}])",
          R"({"stream":"a","data":{},"extra":1})"}) {
        EXPECT_FALSE(s.split(f, e)) << f;
    }
}

TEST(Envelope, FastPathRejectsDuplicateAndTrailingFields) {
    EnvelopeSplitter s;
    Envelope e;
    EXPECT_FALSE(s.split(R"({"stream":"btcusdt@trade","data":{},"data":{}})", e));
    EXPECT_FALSE(s.split(R"({"stream":"btcusdt@trade","data":{},"stream":"other"})", e));
    // Escaped quotes and braces inside data strings must not confuse the scan.
    const std::string tricky =
        R"({"stream":"btcusdt@trade","data":{"a":"x\"}{\\","b":{"c":[1,"]}"]}}})";
    const auto before = s.fallbacks();
    ASSERT_TRUE(s.split(tricky, e));
    EXPECT_EQ(e.data, R"({"a":"x\"}{\\","b":{"c":[1,"]}"]}})");
    EXPECT_EQ(s.fallbacks(), before);  // the tricky frame still takes the fast path
}

TEST(Envelope, WhitespaceGoesThroughFallback) {
    EnvelopeSplitter s;
    Envelope e;
    ASSERT_TRUE(s.split("{ \"stream\" : \"btcusdt@trade\" , \"data\" : {\"x\":1} }", e));
    EXPECT_EQ(e.stream, "btcusdt@trade");
    EXPECT_EQ(e.data, R"({"x":1})");
    EXPECT_EQ(s.fallbacks(), 1u);
}

TEST(Envelope, InvalidInnerJsonIsRejected) {
    EnvelopeSplitter s;
    Envelope e;
    for (const char* data : {R"({"x":nul})", R"({"x":[1,]})", R"({"x":"\q"})", R"({"x":tru})",
                             R"({"x":01})", R"({"x":1.})", R"({"x":-})"}) {
        const std::string fwd = std::string(R"({"stream":"btcusdt@trade","data":)") + data + "}";
        const std::string rev = std::string(R"({"data":)") + data + R"(,"stream":"btcusdt@trade"})";
        EXPECT_FALSE(s.split(fwd, e)) << fwd;
        EXPECT_FALSE(s.split(rev, e)) << rev;
    }
    const std::string huge =
        R"({"stream":"btcusdt@trade","data":{"big":123456789012345678901234567890,"f":-1.5e-300}})";
    EXPECT_TRUE(s.split(huge, e));
}

TEST(Envelope, WorkspaceValidatesAndExtractsRootErrorCode) {
    JsonWorkspace w;
    std::optional<std::int64_t> code;
    ASSERT_TRUE(w.object(R"({"code":-1121,"msg":"Invalid symbol."})", &code));
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, -1121);

    for (const char* doc : {R"({"msg":"x"})", R"({"code":1,"code":2})", R"({"a":{"code":5}})",
                            R"({"code":"5"})", R"({"code":1.5})"}) {
        code.reset();
        EXPECT_TRUE(w.object(doc, &code)) << doc;
        EXPECT_FALSE(code.has_value()) << doc;
    }
    EXPECT_FALSE(w.object(R"({"code":1)", &code));
    EXPECT_FALSE(w.object(R"({"code":1} x)", &code));
    EXPECT_FALSE(w.object("[1]"));
    EXPECT_TRUE(w.object("{}"));
}

TEST(Envelope, OversizedFrameRejected) {
    EnvelopeSplitter s;
    Envelope e;
    std::string big = R"({"stream":"btcusdt@trade","data":{"x":")";
    big.append(std::size_t{1} << 20, 'a');
    big += "\"}}";
    EXPECT_FALSE(s.split(big, e));
}

TEST(Envelope, NestingDepthBounded) {
    JsonWorkspace w;
    std::string deep = "{\"a\":";
    for (int i = 0; i < 200; ++i) {
        deep += "[";
    }
    for (int i = 0; i < 200; ++i) {
        deep += "]";
    }
    deep += "}";
    EXPECT_FALSE(w.object(deep));
}

}  // namespace
}  // namespace blc
