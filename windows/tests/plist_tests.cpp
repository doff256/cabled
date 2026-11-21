#include "plist.h"

#include <gtest/gtest.h>

using cabled::plist::Value;
namespace plist = cabled::plist;

TEST(Plist, RoundTripsNestedValues) {
    Value root = Value::dict();
    root.set("MessageType", Value::string("Connect"));
    root.set("DeviceID", Value::integer(42));
    root.set("Negative", Value::integer(-7));
    root.set("Flag", Value::boolean(true));
    root.set("Ratio", Value::real(0.5));
    root.set("Escaped", Value::string("a<b & c>d"));
    root.set("Blob", Value::data(std::string("\x00\x01\xFF hello", 9)));
    Value list = Value::array();
    list.push(Value::integer(1));
    list.push(Value::string("two"));
    root.set("List", std::move(list));

    std::string error;
    auto parsed = plist::fromXml(plist::toXml(root), &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->find("MessageType")->asString(), "Connect");
    EXPECT_EQ(parsed->find("DeviceID")->asInt(), 42);
    EXPECT_EQ(parsed->find("Negative")->asInt(), -7);
    EXPECT_TRUE(parsed->find("Flag")->asBool());
    EXPECT_DOUBLE_EQ(parsed->find("Ratio")->asReal(), 0.5);
    EXPECT_EQ(parsed->find("Escaped")->asString(), "a<b & c>d");
    EXPECT_EQ(parsed->find("Blob")->asString(), std::string("\x00\x01\xFF hello", 9));
    ASSERT_EQ(parsed->find("List")->items().size(), 2u);
    EXPECT_EQ(parsed->find("List")->items()[1].asString(), "two");
    EXPECT_EQ(parsed->keys().front(), "MessageType");  // insertion order kept
}

TEST(Plist, SetReplacesExistingKey) {
    Value d = Value::dict();
    d.set("A", Value::integer(1));
    d.set("A", Value::integer(2));
    EXPECT_EQ(d.size(), 1u);
    EXPECT_EQ(d.find("A")->asInt(), 2);
    EXPECT_EQ(d.find("Missing"), nullptr);
}

TEST(Plist, ParsesAppleStyleDocumentWithDoctype) {
    const char* xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>MessageType</key>
	<string>Result</string>
	<key>Number</key>
	<integer>3</integer>
</dict>
</plist>
)";
    auto v = plist::fromXml(xml);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->find("MessageType")->asString(), "Result");
    EXPECT_EQ(v->find("Number")->asInt(), 3);
}

TEST(Plist, RejectsGarbage) {
    std::string error;
    EXPECT_FALSE(plist::fromXml("not xml at all", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(plist::fromXml("<plist><dict><string>no key</string></dict></plist>", &error));
}

TEST(Plist, Base64) {
    EXPECT_EQ(plist::base64Encode(""), "");
    EXPECT_EQ(plist::base64Encode("f"), "Zg==");
    EXPECT_EQ(plist::base64Encode("fo"), "Zm8=");
    EXPECT_EQ(plist::base64Encode("foo"), "Zm9v");
    EXPECT_EQ(*plist::base64Decode("Zm9v\nYmFy"), "foobar");
    EXPECT_FALSE(plist::base64Decode("@@@"));
}
