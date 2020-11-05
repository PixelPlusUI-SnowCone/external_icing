// Copyright (C) 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "icing/icing-search-engine.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "icing/jni/jni-cache.h"
#include "icing/text_classifier/lib3/utils/base/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/document-builder.h"
#include "icing/file/filesystem.h"
#include "icing/file/mock-filesystem.h"
#include "icing/helpers/icu/icu-data-file-helper.h"
#include "icing/legacy/index/icing-mock-filesystem.h"
#include "icing/portable/equals-proto.h"
#include "icing/proto/document.pb.h"
#include "icing/proto/initialize.pb.h"
#include "icing/proto/schema.pb.h"
#include "icing/proto/scoring.pb.h"
#include "icing/proto/search.pb.h"
#include "icing/proto/status.pb.h"
#include "icing/schema/schema-store.h"
#include "icing/schema/section.h"
#include "icing/testing/common-matchers.h"
#include "icing/testing/fake-clock.h"
#include "icing/testing/jni-test-helpers.h"
#include "icing/testing/random-string.h"
#include "icing/testing/snippet-helpers.h"
#include "icing/testing/test-data.h"
#include "icing/testing/tmp-directory.h"

namespace icing {
namespace lib {

namespace {

using ::icing::lib::portable_equals_proto::EqualsProto;
using ::testing::_;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Lt;
using ::testing::Matcher;
using ::testing::Ne;
using ::testing::Return;
using ::testing::SizeIs;
using ::testing::StrEq;
using ::testing::UnorderedElementsAre;

constexpr std::string_view kIpsumText =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Nulla convallis "
    "scelerisque orci quis hendrerit. Sed augue turpis, sodales eu gravida "
    "nec, scelerisque nec leo. Maecenas accumsan interdum commodo. Aliquam "
    "mattis sapien est, sit amet interdum risus dapibus sed. Maecenas leo "
    "erat, fringilla in nisl a, venenatis gravida metus. Phasellus venenatis, "
    "orci in aliquet mattis, lectus sapien volutpat arcu, sed hendrerit ligula "
    "arcu nec mauris. Integer dolor mi, rhoncus eget gravida et, pulvinar et "
    "nunc. Aliquam ac sollicitudin nisi. Vivamus sit amet urna vestibulum, "
    "tincidunt eros sed, efficitur nisl. Fusce non neque accumsan, sagittis "
    "nisi eget, sagittis turpis. Ut pulvinar nibh eu purus feugiat faucibus. "
    "Donec tellus nulla, tincidunt vel lacus id, bibendum fermentum turpis. "
    "Nullam ultrices sed nibh vitae aliquet. Ut risus neque, consectetur "
    "vehicula posuere vitae, convallis eu lorem. Donec semper augue eu nibh "
    "placerat semper.";

// For mocking purpose, we allow tests to provide a custom Filesystem.
class TestIcingSearchEngine : public IcingSearchEngine {
 public:
  TestIcingSearchEngine(const IcingSearchEngineOptions& options,
                        std::unique_ptr<const Filesystem> filesystem,
                        std::unique_ptr<const IcingFilesystem> icing_filesystem,
                        std::unique_ptr<FakeClock> clock,
                        std::unique_ptr<JniCache> jni_cache)
      : IcingSearchEngine(options, std::move(filesystem),
                          std::move(icing_filesystem), std::move(clock),
                          std::move(jni_cache)) {}
};

std::string GetTestBaseDir() { return GetTestTempDir() + "/icing"; }

class IcingSearchEngineTest : public testing::Test {
 protected:
  void SetUp() override {
#ifndef ICING_REVERSE_JNI_SEGMENTATION
    // If we've specified using the reverse-JNI method for segmentation (i.e.
    // not ICU), then we won't have the ICU data file included to set up.
    // Technically, we could choose to use reverse-JNI for segmentation AND
    // include an ICU data file, but that seems unlikely and our current BUILD
    // setup doesn't do this.
    // File generated via icu_data_file rule in //icing/BUILD.
    std::string icu_data_file_path =
        GetTestFilePath("icing/icu.dat");
    ICING_ASSERT_OK(icu_data_file_helper::SetUpICUDataFile(icu_data_file_path));
#endif  // ICING_REVERSE_JNI_SEGMENTATION
    filesystem_.CreateDirectoryRecursively(GetTestBaseDir().c_str());
  }

  void TearDown() override {
    filesystem_.DeleteDirectoryRecursively(GetTestBaseDir().c_str());
  }

  const Filesystem* filesystem() const { return &filesystem_; }

 private:
  Filesystem filesystem_;
};

constexpr int kMaxSupportedDocumentSize = (1u << 24) - 1;

// Non-zero value so we don't override it to be the current time
constexpr int64_t kDefaultCreationTimestampMs = 1575492852000;

std::string GetDocumentDir() { return GetTestBaseDir() + "/document_dir"; }

std::string GetIndexDir() { return GetTestBaseDir() + "/index_dir"; }

std::string GetSchemaDir() { return GetTestBaseDir() + "/schema_dir"; }

std::string GetHeaderFilename() {
  return GetTestBaseDir() + "/icing_search_engine_header";
}

IcingSearchEngineOptions GetDefaultIcingOptions() {
  IcingSearchEngineOptions icing_options;
  icing_options.set_base_dir(GetTestBaseDir());
  return icing_options;
}

DocumentProto CreateMessageDocument(std::string name_space, std::string uri) {
  return DocumentBuilder()
      .SetKey(std::move(name_space), std::move(uri))
      .SetSchema("Message")
      .AddStringProperty("body", "message body")
      .SetCreationTimestampMs(kDefaultCreationTimestampMs)
      .Build();
}

SchemaProto CreateMessageSchema() {
  SchemaProto schema;
  auto type = schema.add_types();
  type->set_schema_type("Message");

  auto body = type->add_properties();
  body->set_property_name("body");
  body->set_data_type(PropertyConfigProto::DataType::STRING);
  body->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
  body->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  body->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);

  return schema;
}

SchemaProto CreateEmailSchema() {
  SchemaProto schema;
  auto* type = schema.add_types();
  type->set_schema_type("Email");

  auto* body = type->add_properties();
  body->set_property_name("body");
  body->set_data_type(PropertyConfigProto::DataType::STRING);
  body->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
  body->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  body->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);
  auto* subj = type->add_properties();
  subj->set_property_name("subject");
  subj->set_data_type(PropertyConfigProto::DataType::STRING);
  subj->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
  subj->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  subj->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);

  return schema;
}

ScoringSpecProto GetDefaultScoringSpec() {
  ScoringSpecProto scoring_spec;
  scoring_spec.set_rank_by(ScoringSpecProto::RankingStrategy::DOCUMENT_SCORE);
  return scoring_spec;
}

UsageReport CreateUsageReport(std::string name_space, std::string uri,
                              int64 timestamp_ms,
                              UsageReport::UsageType usage_type) {
  UsageReport usage_report;
  usage_report.set_document_namespace(name_space);
  usage_report.set_document_uri(uri);
  usage_report.set_usage_timestamp_ms(timestamp_ms);
  usage_report.set_usage_type(usage_type);
  return usage_report;
}

TEST_F(IcingSearchEngineTest, SimpleInitialization) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document = CreateMessageDocument("namespace", "uri");
  ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(DocumentProto(document)).status(), ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, InitializingAgainSavesNonPersistedData) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document = CreateMessageDocument("namespace", "uri");
  ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document;

  ASSERT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));

  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));
}

TEST_F(IcingSearchEngineTest, MaxIndexMergeSizeReturnsInvalidArgument) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  options.set_index_merge_size(std::numeric_limits<int32_t>::max());
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
}

TEST_F(IcingSearchEngineTest, NegativeMergeSizeReturnsInvalidArgument) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  options.set_index_merge_size(-1);
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
}

TEST_F(IcingSearchEngineTest, ZeroMergeSizeReturnsInvalidArgument) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  options.set_index_merge_size(0);
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
}

TEST_F(IcingSearchEngineTest, GoodIndexMergeSizeReturnsOk) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  // One is fine, if a bit weird. It just means that the lite index will be
  // smaller and will request a merge any time content is added to it.
  options.set_index_merge_size(1);
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
}

TEST_F(IcingSearchEngineTest,
       NegativeMaxTokensPerDocSizeReturnsInvalidArgument) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  options.set_max_tokens_per_doc(-1);
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
}

TEST_F(IcingSearchEngineTest, ZeroMaxTokensPerDocSizeReturnsInvalidArgument) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  options.set_max_tokens_per_doc(0);
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
}

TEST_F(IcingSearchEngineTest, GoodMaxTokensPerDocSizeReturnsOk) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  // INT_MAX is valid - it just means that we shouldn't limit the number of
  // tokens per document. It would be pretty inconceivable that anyone would
  // produce such a document - the text being indexed alone would take up at
  // least ~4.3 GiB! - and the document would be rejected before indexing
  // for exceeding max_document_size, but there's no reason to explicitly
  // bar it.
  options.set_max_tokens_per_doc(std::numeric_limits<int32_t>::max());
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, NegativeMaxTokenLenReturnsInvalidArgument) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  options.set_max_token_length(-1);
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
}

TEST_F(IcingSearchEngineTest, ZeroMaxTokenLenReturnsInvalidArgument) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  options.set_max_token_length(0);
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
}

TEST_F(IcingSearchEngineTest, MaxTokenLenReturnsOkAndTruncatesTokens) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  // A length of 1 is allowed - even though it would be strange to want
  // this.
  options.set_max_token_length(1);
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document = CreateMessageDocument("namespace", "uri");
  EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());

  // "message" should have been truncated to "m"
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  // The indexed tokens were  truncated to length of 1, so "m" will match
  search_spec.set_query("m");

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document;

  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  // The query token is also truncated to length of 1, so "me"->"m" matches "m"
  search_spec.set_query("me");
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  // The query token is still truncated to length of 1, so "massage"->"m"
  // matches "m"
  search_spec.set_query("massage");
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest,
       MaxIntMaxTokenLenReturnsOkTooLargeTokenReturnsResourceExhausted) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  // Set token length to max. This is allowed (it just means never to
  // truncate tokens). However, this does mean that tokens that exceed the
  // size of the lexicon will cause indexing to fail.
  options.set_max_token_length(std::numeric_limits<int32_t>::max());
  IcingSearchEngine icing(options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Add a document that just barely fits under the max document limit.
  // This will still fail to index because we won't actually have enough
  // room in the lexicon to fit this content.
  std::string enormous_string(kMaxSupportedDocumentSize - 256, 'p');
  DocumentProto document =
      DocumentBuilder()
          .SetKey("namespace", "uri")
          .SetSchema("Message")
          .AddStringProperty("body", std::move(enormous_string))
          .Build();
  EXPECT_THAT(icing.Put(document).status(),
              ProtoStatusIs(StatusProto::OUT_OF_SPACE));

  SearchSpecProto search_spec;
  search_spec.set_query("p");
  search_spec.set_term_match_type(TermMatchType::PREFIX);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, FailToCreateDocStore) {
  auto mock_filesystem = std::make_unique<MockFilesystem>();
  // This fails DocumentStore::Create()
  ON_CALL(*mock_filesystem, CreateDirectoryRecursively(_))
      .WillByDefault(Return(false));

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());

  InitializeResultProto initialize_result_proto = icing.Initialize();
  EXPECT_THAT(initialize_result_proto.status(),
              ProtoStatusIs(StatusProto::INTERNAL));
  EXPECT_THAT(initialize_result_proto.status().message(),
              HasSubstr("Could not create directory"));
}

TEST_F(IcingSearchEngineTest,
       CircularReferenceCreateSectionManagerReturnsInvalidArgument) {
  // Create a type config with a circular reference.
  SchemaProto schema;
  auto* type = schema.add_types();
  type->set_schema_type("Message");

  auto* body = type->add_properties();
  body->set_property_name("recipient");
  body->set_schema_type("Person");
  body->set_data_type(PropertyConfigProto::DataType::DOCUMENT);
  body->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
  body->mutable_document_indexing_config()->set_index_nested_properties(true);

  type = schema.add_types();
  type->set_schema_type("Person");

  body = type->add_properties();
  body->set_property_name("recipient");
  body->set_schema_type("Message");
  body->set_data_type(PropertyConfigProto::DataType::DOCUMENT);
  body->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
  body->mutable_document_indexing_config()->set_index_nested_properties(true);

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(schema).status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
}

TEST_F(IcingSearchEngineTest, PutWithoutSchemaFailedPrecondition) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  DocumentProto document = CreateMessageDocument("namespace", "uri");
  PutResultProto put_result_proto = icing.Put(document);
  EXPECT_THAT(put_result_proto.status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(put_result_proto.status().message(), HasSubstr("Schema not set"));
}

TEST_F(IcingSearchEngineTest, FailToReadSchema) {
  IcingSearchEngineOptions icing_options = GetDefaultIcingOptions();

  {
    // Successfully initialize and set a schema
    IcingSearchEngine icing(icing_options, GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  }

  auto mock_filesystem = std::make_unique<MockFilesystem>();

  // This fails FileBackedProto::Read() when we try to check the schema we
  // had previously set
  ON_CALL(*mock_filesystem,
          OpenForRead(Eq(icing_options.base_dir() + "/schema_dir/schema.pb")))
      .WillByDefault(Return(-1));

  TestIcingSearchEngine test_icing(icing_options, std::move(mock_filesystem),
                                   std::make_unique<IcingFilesystem>(),
                                   std::make_unique<FakeClock>(),
                                   GetTestJniCache());

  InitializeResultProto initialize_result_proto = test_icing.Initialize();
  EXPECT_THAT(initialize_result_proto.status(),
              ProtoStatusIs(StatusProto::INTERNAL));
  EXPECT_THAT(initialize_result_proto.status().message(),
              HasSubstr("Unable to open file for read"));
}

TEST_F(IcingSearchEngineTest, FailToWriteSchema) {
  IcingSearchEngineOptions icing_options = GetDefaultIcingOptions();

  auto mock_filesystem = std::make_unique<MockFilesystem>();
  // This fails FileBackedProto::Write()
  ON_CALL(*mock_filesystem,
          OpenForWrite(Eq(icing_options.base_dir() + "/schema_dir/schema.pb")))
      .WillByDefault(Return(-1));

  TestIcingSearchEngine icing(icing_options, std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());

  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

  SetSchemaResultProto set_schema_result_proto =
      icing.SetSchema(CreateMessageSchema());
  EXPECT_THAT(set_schema_result_proto.status(),
              ProtoStatusIs(StatusProto::INTERNAL));
  EXPECT_THAT(set_schema_result_proto.status().message(),
              HasSubstr("Unable to open file for write"));
}

TEST_F(IcingSearchEngineTest, SetSchemaDelete2) {
  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    // 1. Create a schema with an Email type with properties { "title", "body"}
    SchemaProto schema;
    SchemaTypeConfigProto* type = schema.add_types();
    type->set_schema_type("Email");
    PropertyConfigProto* property = type->add_properties();
    property->set_property_name("title");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
    property = type->add_properties();
    property->set_property_name("body");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

    EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());

    // 2. Add an email document
    DocumentProto doc = DocumentBuilder()
                            .SetKey("emails", "email#1")
                            .SetSchema("Email")
                            .AddStringProperty("title", "Hello world.")
                            .AddStringProperty("body", "Goodnight Moon.")
                            .Build();
    EXPECT_THAT(icing.Put(std::move(doc)).status(), ProtoIsOk());
  }

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    // 3. Set a schema that deletes email. This should fail.
    SchemaProto schema;
    SchemaTypeConfigProto* type = schema.add_types();
    type->set_schema_type("Message");
    PropertyConfigProto* property = type->add_properties();
    property->set_property_name("body");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

    EXPECT_THAT(icing.SetSchema(schema, false).status(),
                ProtoStatusIs(StatusProto::FAILED_PRECONDITION));

    // 4. Try to delete by email type.
    EXPECT_THAT(icing.DeleteBySchemaType("Email").status(), ProtoIsOk());
  }
}

TEST_F(IcingSearchEngineTest, SetSchemaDelete) {
  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    // 1. Create a schema with an Email type with properties { "title", "body"}
    SchemaProto schema;
    SchemaTypeConfigProto* type = schema.add_types();
    type->set_schema_type("Email");
    PropertyConfigProto* property = type->add_properties();
    property->set_property_name("title");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
    property = type->add_properties();
    property->set_property_name("body");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

    EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());

    // 2. Add an email document
    DocumentProto doc = DocumentBuilder()
                            .SetKey("emails", "email#1")
                            .SetSchema("Email")
                            .AddStringProperty("title", "Hello world.")
                            .AddStringProperty("body", "Goodnight Moon.")
                            .Build();
    EXPECT_THAT(icing.Put(std::move(doc)).status(), ProtoIsOk());
  }

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    // 3. Set a schema that deletes email. This should fail.
    SchemaProto schema;
    SchemaTypeConfigProto* type = schema.add_types();
    type->set_schema_type("Message");
    PropertyConfigProto* property = type->add_properties();
    property->set_property_name("body");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

    EXPECT_THAT(icing.SetSchema(schema, true).status(), ProtoIsOk());

    // 4. Try to delete by email type.
    EXPECT_THAT(icing.DeleteBySchemaType("Email").status(),
                ProtoStatusIs(StatusProto::NOT_FOUND));
  }
}

TEST_F(IcingSearchEngineTest, SetSchemaDuplicateTypesReturnsAlreadyExists) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  // Create a schema with types { "Email", "Message" and "Email" }
  SchemaProto schema;
  SchemaTypeConfigProto* type = schema.add_types();
  type->set_schema_type("Email");
  PropertyConfigProto* property = type->add_properties();
  property->set_property_name("title");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  type = schema.add_types();
  type->set_schema_type("Message");
  property = type->add_properties();
  property->set_property_name("body");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  *schema.add_types() = schema.types(0);

  EXPECT_THAT(icing.SetSchema(schema).status(),
              ProtoStatusIs(StatusProto::ALREADY_EXISTS));
}

TEST_F(IcingSearchEngineTest,
       SetSchemaDuplicatePropertiesReturnsAlreadyExists) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  // Create a schema with an Email type with properties { "title", "body" and
  // "title" }
  SchemaProto schema;
  SchemaTypeConfigProto* type = schema.add_types();
  type->set_schema_type("Email");
  PropertyConfigProto* property = type->add_properties();
  property->set_property_name("title");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
  property = type->add_properties();
  property->set_property_name("body");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
  property = type->add_properties();
  property->set_property_name("title");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  EXPECT_THAT(icing.SetSchema(schema).status(),
              ProtoStatusIs(StatusProto::ALREADY_EXISTS));
}

TEST_F(IcingSearchEngineTest, SetSchema) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  auto message_document = CreateMessageDocument("namespace", "uri");

  auto schema_with_message = CreateMessageSchema();

  SchemaProto schema_with_email;
  SchemaTypeConfigProto* type = schema_with_email.add_types();
  type->set_schema_type("Email");
  PropertyConfigProto* property = type->add_properties();
  property->set_property_name("title");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  SchemaProto schema_with_email_and_message = schema_with_email;
  type = schema_with_email_and_message.add_types();
  type->set_schema_type("Message");
  property = type->add_properties();
  property->set_property_name("body");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  // Create an arbitrary invalid schema
  SchemaProto invalid_schema;
  SchemaTypeConfigProto* empty_type = invalid_schema.add_types();
  empty_type->set_schema_type("");

  // Make sure we can't set invalid schemas
  EXPECT_THAT(icing.SetSchema(invalid_schema).status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));

  // Can add an document of a set schema
  EXPECT_THAT(icing.SetSchema(schema_with_message).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(message_document).status(), ProtoIsOk());

  // Schema with Email doesn't have Message, so would result incompatible
  // data
  EXPECT_THAT(icing.SetSchema(schema_with_email).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));

  // Can expand the set of schema types and add an document of a new
  // schema type
  EXPECT_THAT(icing.SetSchema(SchemaProto(schema_with_email_and_message))
                  .status()
                  .code(),
              Eq(StatusProto::OK));
  EXPECT_THAT(icing.Put(message_document).status(), ProtoIsOk());

  // Can't add an document whose schema isn't set
  auto photo_document = DocumentBuilder()
                            .SetKey("namespace", "uri")
                            .SetSchema("Photo")
                            .AddStringProperty("creator", "icing")
                            .Build();
  PutResultProto put_result_proto = icing.Put(photo_document);
  EXPECT_THAT(put_result_proto.status(), ProtoStatusIs(StatusProto::NOT_FOUND));
  EXPECT_THAT(put_result_proto.status().message(),
              HasSubstr("'Photo' not found"));
}

TEST_F(IcingSearchEngineTest, SetSchemaTriggersIndexRestorationAndReturnsOk) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  SchemaProto schema_with_no_indexed_property = CreateMessageSchema();
  schema_with_no_indexed_property.mutable_types(0)
      ->mutable_properties(0)
      ->clear_string_indexing_config();

  EXPECT_THAT(icing.SetSchema(schema_with_no_indexed_property).status(),
              ProtoIsOk());
  // Nothing will be index and Search() won't return anything.
  EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
              ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto empty_result;
  empty_result.mutable_status()->set_code(StatusProto::OK);

  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(empty_result));

  SchemaProto schema_with_indexed_property = CreateMessageSchema();
  // Index restoration should be triggered here because new schema requires more
  // properties to be indexed.
  EXPECT_THAT(icing.SetSchema(schema_with_indexed_property).status(),
              ProtoIsOk());

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      CreateMessageDocument("namespace", "uri");
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SetSchemaRevalidatesDocumentsAndReturnsOk) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  SchemaProto schema_with_optional_subject;
  auto type = schema_with_optional_subject.add_types();
  type->set_schema_type("email");

  // Add a OPTIONAL property
  auto property = type->add_properties();
  property->set_property_name("subject");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  EXPECT_THAT(icing.SetSchema(schema_with_optional_subject).status(),
              ProtoIsOk());

  DocumentProto email_document_without_subject =
      DocumentBuilder()
          .SetKey("namespace", "without_subject")
          .SetSchema("email")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto email_document_with_subject =
      DocumentBuilder()
          .SetKey("namespace", "with_subject")
          .SetSchema("email")
          .AddStringProperty("subject", "foo")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  EXPECT_THAT(icing.Put(email_document_without_subject).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(email_document_with_subject).status(), ProtoIsOk());

  SchemaProto schema_with_required_subject;
  type = schema_with_required_subject.add_types();
  type->set_schema_type("email");

  // Add a REQUIRED property
  property = type->add_properties();
  property->set_property_name("subject");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);

  // Can't set the schema since it's incompatible
  SetSchemaResultProto expected_set_schema_result_proto;
  expected_set_schema_result_proto.mutable_status()->set_code(
      StatusProto::FAILED_PRECONDITION);
  expected_set_schema_result_proto.mutable_status()->set_message(
      "Schema is incompatible.");
  expected_set_schema_result_proto.add_incompatible_schema_types("email");

  EXPECT_THAT(icing.SetSchema(schema_with_required_subject),
              EqualsProto(expected_set_schema_result_proto));

  // Force set it
  expected_set_schema_result_proto.mutable_status()->set_code(StatusProto::OK);
  expected_set_schema_result_proto.mutable_status()->clear_message();
  EXPECT_THAT(icing.SetSchema(schema_with_required_subject,
                              /*ignore_errors_and_delete_documents=*/true),
              EqualsProto(expected_set_schema_result_proto));

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = email_document_with_subject;

  EXPECT_THAT(icing.Get("namespace", "with_subject"),
              EqualsProto(expected_get_result_proto));

  // The document without a subject got deleted because it failed validation
  // against the new schema
  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace, without_subject) not found.");
  expected_get_result_proto.clear_document();

  EXPECT_THAT(icing.Get("namespace", "without_subject"),
              EqualsProto(expected_get_result_proto));
}

TEST_F(IcingSearchEngineTest, SetSchemaDeletesDocumentsAndReturnsOk) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  SchemaProto schema;
  auto type = schema.add_types();
  type->set_schema_type("email");
  type = schema.add_types();
  type->set_schema_type("message");

  EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());

  DocumentProto email_document =
      DocumentBuilder()
          .SetKey("namespace", "email_uri")
          .SetSchema("email")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto message_document =
      DocumentBuilder()
          .SetKey("namespace", "message_uri")
          .SetSchema("message")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  EXPECT_THAT(icing.Put(email_document).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(message_document).status(), ProtoIsOk());

  // Clear the schema and only add the "email" type, essentially deleting the
  // "message" type
  SchemaProto new_schema;
  type = new_schema.add_types();
  type->set_schema_type("email");

  // Can't set the schema since it's incompatible
  SetSchemaResultProto expected_result;
  expected_result.mutable_status()->set_code(StatusProto::FAILED_PRECONDITION);
  expected_result.mutable_status()->set_message("Schema is incompatible.");
  expected_result.add_deleted_schema_types("message");

  EXPECT_THAT(icing.SetSchema(new_schema), EqualsProto(expected_result));

  // Force set it
  expected_result.mutable_status()->set_code(StatusProto::OK);
  expected_result.mutable_status()->clear_message();
  EXPECT_THAT(icing.SetSchema(new_schema,
                              /*ignore_errors_and_delete_documents=*/true),
              EqualsProto(expected_result));

  // "email" document is still there
  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = email_document;

  EXPECT_THAT(icing.Get("namespace", "email_uri"),
              EqualsProto(expected_get_result_proto));

  // "message" document got deleted
  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace, message_uri) not found.");
  expected_get_result_proto.clear_document();

  EXPECT_THAT(icing.Get("namespace", "message_uri"),
              EqualsProto(expected_get_result_proto));
}

TEST_F(IcingSearchEngineTest, GetSchemaNotFound) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  EXPECT_THAT(icing.GetSchema().status(),
              ProtoStatusIs(StatusProto::NOT_FOUND));
}

TEST_F(IcingSearchEngineTest, GetSchemaOk) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  GetSchemaResultProto expected_get_schema_result_proto;
  expected_get_schema_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_schema_result_proto.mutable_schema() = CreateMessageSchema();
  EXPECT_THAT(icing.GetSchema(), EqualsProto(expected_get_schema_result_proto));
}

TEST_F(IcingSearchEngineTest, GetSchemaTypeFailedPrecondition) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  GetSchemaTypeResultProto get_schema_type_result_proto =
      icing.GetSchemaType("nonexistent_schema");
  EXPECT_THAT(get_schema_type_result_proto.status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(get_schema_type_result_proto.status().message(),
              HasSubstr("Schema not set"));
}

TEST_F(IcingSearchEngineTest, GetSchemaTypeOk) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  GetSchemaTypeResultProto expected_get_schema_type_result_proto;
  expected_get_schema_type_result_proto.mutable_status()->set_code(
      StatusProto::OK);
  *expected_get_schema_type_result_proto.mutable_schema_type_config() =
      CreateMessageSchema().types(0);
  EXPECT_THAT(icing.GetSchemaType(CreateMessageSchema().types(0).schema_type()),
              EqualsProto(expected_get_schema_type_result_proto));
}

TEST_F(IcingSearchEngineTest, GetDocument) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Simple put and get
  ASSERT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
              ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() =
      CreateMessageDocument("namespace", "uri");
  ASSERT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));

  // Put an invalid document
  PutResultProto put_result_proto = icing.Put(DocumentProto());
  EXPECT_THAT(put_result_proto.status(),
              ProtoStatusIs(StatusProto::INVALID_ARGUMENT));
  EXPECT_THAT(put_result_proto.status().message(),
              HasSubstr("'namespace' is empty"));

  // Get a non-existing key
  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (wrong, uri) not found.");
  expected_get_result_proto.clear_document();
  ASSERT_THAT(icing.Get("wrong", "uri"),
              EqualsProto(expected_get_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchReturnsValidResults) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document_one = CreateMessageDocument("namespace", "uri1");
  ASSERT_THAT(icing.Put(document_one).status(), ProtoIsOk());

  DocumentProto document_two = CreateMessageDocument("namespace", "uri2");
  ASSERT_THAT(icing.Put(document_two).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("message");

  ResultSpecProto result_spec;
  result_spec.mutable_snippet_spec()->set_max_window_bytes(64);
  result_spec.mutable_snippet_spec()->set_num_matches_per_property(1);
  result_spec.mutable_snippet_spec()->set_num_to_snippet(1);

  SearchResultProto results =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  EXPECT_THAT(results.status(), ProtoIsOk());
  EXPECT_THAT(results.results(), SizeIs(2));
  EXPECT_THAT(results.results(0).document(), EqualsProto(document_two));
  EXPECT_THAT(GetMatch(results.results(0).document(),
                       results.results(0).snippet(), "body",
                       /*snippet_index=*/0),
              Eq("message"));
  EXPECT_THAT(
      GetWindow(results.results(0).document(), results.results(0).snippet(),
                "body", /*snippet_index=*/0),
      Eq("message body"));
  EXPECT_THAT(results.results(1).document(), EqualsProto(document_one));
  EXPECT_THAT(
      GetMatch(results.results(1).document(), results.results(1).snippet(),
               "body", /*snippet_index=*/0),
      IsEmpty());
  EXPECT_THAT(
      GetWindow(results.results(1).document(), results.results(1).snippet(),
                "body", /*snippet_index=*/0),
      IsEmpty());

  search_spec.set_query("foo");

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchReturnsOneResult) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document_one = CreateMessageDocument("namespace", "uri1");
  ASSERT_THAT(icing.Put(document_one).status(), ProtoIsOk());

  DocumentProto document_two = CreateMessageDocument("namespace", "uri2");
  ASSERT_THAT(icing.Put(document_two).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("message");

  ResultSpecProto result_spec;
  result_spec.set_num_per_page(1);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document_two;

  SearchResultProto search_result_proto =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  EXPECT_THAT(search_result_proto.status(), ProtoIsOk());
  // The token is a random number so we don't verify it.
  expected_search_result_proto.set_next_page_token(
      search_result_proto.next_page_token());
  EXPECT_THAT(search_result_proto, EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchZeroResultLimitReturnsEmptyResults) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("");

  ResultSpecProto result_spec;
  result_spec.set_num_per_page(0);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(), result_spec),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchNegativeResultLimitReturnsInvalidArgument) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("");

  ResultSpecProto result_spec;
  result_spec.set_num_per_page(-5);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(
      StatusProto::INVALID_ARGUMENT);
  expected_search_result_proto.mutable_status()->set_message(
      "ResultSpecProto.num_per_page cannot be negative.");
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(), result_spec),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchWithPersistenceReturnsValidResults) {
  IcingSearchEngineOptions icing_options = GetDefaultIcingOptions();

  {
    // Set the schema up beforehand.
    IcingSearchEngine icing(icing_options, GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    // Schema will be persisted to disk when icing goes out of scope.
  }

  {
    // Ensure that icing initializes the schema and section_manager
    // properly from the pre-existing file.
    IcingSearchEngine icing(icing_options, GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());
    // The index and document store will be persisted to disk when icing goes
    // out of scope.
  }

  {
    // Ensure that the index is brought back up without problems and we
    // can query for the content that we expect.
    IcingSearchEngine icing(icing_options, GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    SearchSpecProto search_spec;
    search_spec.set_term_match_type(TermMatchType::PREFIX);
    search_spec.set_query("message");

    SearchResultProto expected_search_result_proto;
    expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
    *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
        CreateMessageDocument("namespace", "uri");

    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));

    search_spec.set_query("foo");

    SearchResultProto empty_result;
    empty_result.mutable_status()->set_code(StatusProto::OK);
    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(empty_result));
  }
}

TEST_F(IcingSearchEngineTest, SearchShouldReturnEmpty) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("message");

  // Empty result, no next-page token
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);

  SearchResultProto search_result_proto =
      icing.Search(search_spec, GetDefaultScoringSpec(),
                   ResultSpecProto::default_instance());

  EXPECT_THAT(search_result_proto, EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchShouldReturnMultiplePages) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates and inserts 5 documents
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");
  DocumentProto document3 = CreateMessageDocument("namespace", "uri3");
  DocumentProto document4 = CreateMessageDocument("namespace", "uri4");
  DocumentProto document5 = CreateMessageDocument("namespace", "uri5");
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document4).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document5).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("message");

  ResultSpecProto result_spec;
  result_spec.set_num_per_page(2);

  // Searches and gets the first page, 2 results
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document5;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document4;
  SearchResultProto search_result_proto =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  EXPECT_THAT(search_result_proto.next_page_token(), Gt(kInvalidNextPageToken));
  uint64_t next_page_token = search_result_proto.next_page_token();
  // Since the token is a random number, we don't need to verify
  expected_search_result_proto.set_next_page_token(next_page_token);
  EXPECT_THAT(search_result_proto, EqualsProto(expected_search_result_proto));

  // Second page, 2 results
  expected_search_result_proto.clear_results();
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  EXPECT_THAT(icing.GetNextPage(next_page_token),
              EqualsProto(expected_search_result_proto));

  // Third page, 1 result
  expected_search_result_proto.clear_results();
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;
  // Because there are no more results, we should not return the next page
  // token.
  expected_search_result_proto.clear_next_page_token();
  EXPECT_THAT(icing.GetNextPage(next_page_token),
              EqualsProto(expected_search_result_proto));

  // No more results
  expected_search_result_proto.clear_results();
  EXPECT_THAT(icing.GetNextPage(next_page_token),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchWithNoScoringShouldReturnMultiplePages) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates and inserts 5 documents
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");
  DocumentProto document3 = CreateMessageDocument("namespace", "uri3");
  DocumentProto document4 = CreateMessageDocument("namespace", "uri4");
  DocumentProto document5 = CreateMessageDocument("namespace", "uri5");
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document4).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document5).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("message");

  ScoringSpecProto scoring_spec;
  scoring_spec.set_rank_by(ScoringSpecProto::RankingStrategy::NONE);

  ResultSpecProto result_spec;
  result_spec.set_num_per_page(2);

  // Searches and gets the first page, 2 results
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document5;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document4;
  SearchResultProto search_result_proto =
      icing.Search(search_spec, scoring_spec, result_spec);
  EXPECT_THAT(search_result_proto.next_page_token(), Gt(kInvalidNextPageToken));
  uint64_t next_page_token = search_result_proto.next_page_token();
  // Since the token is a random number, we don't need to verify
  expected_search_result_proto.set_next_page_token(next_page_token);
  EXPECT_THAT(search_result_proto, EqualsProto(expected_search_result_proto));

  // Second page, 2 results
  expected_search_result_proto.clear_results();
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  EXPECT_THAT(icing.GetNextPage(next_page_token),
              EqualsProto(expected_search_result_proto));

  // Third page, 1 result
  expected_search_result_proto.clear_results();
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;
  // Because there are no more results, we should not return the next page
  // token.
  expected_search_result_proto.clear_next_page_token();
  EXPECT_THAT(icing.GetNextPage(next_page_token),
              EqualsProto(expected_search_result_proto));

  // No more results
  expected_search_result_proto.clear_results();
  EXPECT_THAT(icing.GetNextPage(next_page_token),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, ShouldReturnMultiplePagesWithSnippets) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates and inserts 5 documents
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");
  DocumentProto document3 = CreateMessageDocument("namespace", "uri3");
  DocumentProto document4 = CreateMessageDocument("namespace", "uri4");
  DocumentProto document5 = CreateMessageDocument("namespace", "uri5");
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document4).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document5).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("message");

  ResultSpecProto result_spec;
  result_spec.set_num_per_page(2);
  result_spec.mutable_snippet_spec()->set_max_window_bytes(64);
  result_spec.mutable_snippet_spec()->set_num_matches_per_property(1);
  result_spec.mutable_snippet_spec()->set_num_to_snippet(3);

  // Searches and gets the first page, 2 results with 2 snippets
  SearchResultProto search_result =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  ASSERT_THAT(search_result.status(), ProtoIsOk());
  ASSERT_THAT(search_result.results(), SizeIs(2));
  ASSERT_THAT(search_result.next_page_token(), Gt(kInvalidNextPageToken));

  EXPECT_THAT(search_result.results(0).document(), EqualsProto(document5));
  EXPECT_THAT(GetMatch(search_result.results(0).document(),
                       search_result.results(0).snippet(), "body",
                       /*snippet_index=*/0),
              Eq("message"));
  EXPECT_THAT(GetWindow(search_result.results(0).document(),
                        search_result.results(0).snippet(), "body",
                        /*snippet_index=*/0),
              Eq("message body"));
  EXPECT_THAT(search_result.results(1).document(), EqualsProto(document4));
  EXPECT_THAT(GetMatch(search_result.results(1).document(),
                       search_result.results(1).snippet(), "body",
                       /*snippet_index=*/0),
              Eq("message"));
  EXPECT_THAT(GetWindow(search_result.results(1).document(),
                        search_result.results(1).snippet(), "body",
                        /*snippet_index=*/0),
              Eq("message body"));

  // Second page, 2 result with 1 snippet
  search_result = icing.GetNextPage(search_result.next_page_token());
  ASSERT_THAT(search_result.status(), ProtoIsOk());
  ASSERT_THAT(search_result.results(), SizeIs(2));
  ASSERT_THAT(search_result.next_page_token(), Gt(kInvalidNextPageToken));

  EXPECT_THAT(search_result.results(0).document(), EqualsProto(document3));
  EXPECT_THAT(GetMatch(search_result.results(0).document(),
                       search_result.results(0).snippet(), "body",
                       /*snippet_index=*/0),
              Eq("message"));
  EXPECT_THAT(GetWindow(search_result.results(0).document(),
                        search_result.results(0).snippet(), "body",
                        /*snippet_index=*/0),
              Eq("message body"));
  EXPECT_THAT(search_result.results(1).document(), EqualsProto(document2));
  EXPECT_THAT(search_result.results(1).snippet().entries_size(), Eq(0));

  // Third page, 1 result with 0 snippets
  search_result = icing.GetNextPage(search_result.next_page_token());
  ASSERT_THAT(search_result.status(), ProtoIsOk());
  ASSERT_THAT(search_result.results(), SizeIs(1));
  ASSERT_THAT(search_result.next_page_token(), Eq(kInvalidNextPageToken));

  EXPECT_THAT(search_result.results(0).document(), EqualsProto(document1));
  EXPECT_THAT(search_result.results(0).snippet().entries_size(), Eq(0));
}

TEST_F(IcingSearchEngineTest, ShouldInvalidateNextPageToken) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("message");

  ResultSpecProto result_spec;
  result_spec.set_num_per_page(1);

  // Searches and gets the first page, 1 result
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  SearchResultProto search_result_proto =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  EXPECT_THAT(search_result_proto.next_page_token(), Gt(kInvalidNextPageToken));
  uint64_t next_page_token = search_result_proto.next_page_token();
  // Since the token is a random number, we don't need to verify
  expected_search_result_proto.set_next_page_token(next_page_token);
  EXPECT_THAT(search_result_proto, EqualsProto(expected_search_result_proto));
  // Now document1 is still to be fetched.

  // Invalidates token
  icing.InvalidateNextPageToken(next_page_token);

  // Tries to fetch the second page, no result since it's invalidated
  expected_search_result_proto.clear_results();
  expected_search_result_proto.clear_next_page_token();
  EXPECT_THAT(icing.GetNextPage(next_page_token),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest,
       AllPageTokensShouldBeInvalidatedAfterOptimization) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("message");

  ResultSpecProto result_spec;
  result_spec.set_num_per_page(1);

  // Searches and gets the first page, 1 result
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  SearchResultProto search_result_proto =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  EXPECT_THAT(search_result_proto.next_page_token(), Gt(kInvalidNextPageToken));
  uint64_t next_page_token = search_result_proto.next_page_token();
  // Since the token is a random number, we don't need to verify
  expected_search_result_proto.set_next_page_token(next_page_token);
  EXPECT_THAT(search_result_proto, EqualsProto(expected_search_result_proto));
  // Now document1 is still to be fetched.

  OptimizeResultProto optimize_result_proto;
  optimize_result_proto.mutable_status()->set_code(StatusProto::OK);
  optimize_result_proto.mutable_status()->set_message("");
  ASSERT_THAT(icing.Optimize(), EqualsProto(optimize_result_proto));

  // Tries to fetch the second page, no results since all tokens have been
  // invalidated during Optimize()
  expected_search_result_proto.clear_results();
  expected_search_result_proto.clear_next_page_token();
  EXPECT_THAT(icing.GetNextPage(next_page_token),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, OptimizationShouldRemoveDeletedDocs) {
  IcingSearchEngineOptions icing_options = GetDefaultIcingOptions();

  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace, uri1) not found.");
  {
    IcingSearchEngine icing(icing_options, GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());

    // Deletes document1
    ASSERT_THAT(icing.Delete("namespace", "uri1").status(), ProtoIsOk());
    const std::string document_log_path =
        icing_options.base_dir() + "/document_dir/document_log";
    int64_t document_log_size_before =
        filesystem()->GetFileSize(document_log_path.c_str());
    ASSERT_THAT(icing.Optimize().status(), ProtoIsOk());
    int64_t document_log_size_after =
        filesystem()->GetFileSize(document_log_path.c_str());

    // Validates that document can't be found right after Optimize()
    EXPECT_THAT(icing.Get("namespace", "uri1"),
                EqualsProto(expected_get_result_proto));
    // Validates that document is actually removed from document log
    EXPECT_THAT(document_log_size_after, Lt(document_log_size_before));
  }  // Destroys IcingSearchEngine to make sure nothing is cached.

  IcingSearchEngine icing(icing_options, GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.Get("namespace", "uri1"),
              EqualsProto(expected_get_result_proto));
}

TEST_F(IcingSearchEngineTest, OptimizationShouldDeleteTemporaryDirectory) {
  IcingSearchEngineOptions icing_options = GetDefaultIcingOptions();
  IcingSearchEngine icing(icing_options, GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Create a tmp dir that will be used in Optimize() to swap files,
  // this validates that any tmp dirs will be deleted before using.
  const std::string tmp_dir =
      icing_options.base_dir() + "/document_dir_optimize_tmp";

  const std::string tmp_file = tmp_dir + "/file";
  ASSERT_TRUE(filesystem()->CreateDirectory(tmp_dir.c_str()));
  ScopedFd fd(filesystem()->OpenForWrite(tmp_file.c_str()));
  ASSERT_TRUE(fd.is_valid());
  ASSERT_TRUE(filesystem()->Write(fd.get(), "1234", 4));
  fd.reset();

  EXPECT_THAT(icing.Optimize().status(), ProtoIsOk());

  EXPECT_FALSE(filesystem()->DirectoryExists(tmp_dir.c_str()));
  EXPECT_FALSE(filesystem()->FileExists(tmp_file.c_str()));
}

TEST_F(IcingSearchEngineTest, GetOptimizeInfoHasCorrectStats) {
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = DocumentBuilder()
                                .SetKey("namespace", "uri2")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message body")
                                .SetCreationTimestampMs(100)
                                .SetTtlMs(500)
                                .Build();

  auto fake_clock = std::make_unique<FakeClock>();
  fake_clock->SetSystemTimeMilliseconds(1000);

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::make_unique<Filesystem>(),
                              std::make_unique<IcingFilesystem>(),
                              std::move(fake_clock), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  // Just initialized, nothing is optimizable yet.
  GetOptimizeInfoResultProto optimize_info = icing.GetOptimizeInfo();
  EXPECT_THAT(optimize_info.status(), ProtoIsOk());
  EXPECT_THAT(optimize_info.optimizable_docs(), Eq(0));
  EXPECT_THAT(optimize_info.estimated_optimizable_bytes(), Eq(0));

  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());

  // Only have active documents, nothing is optimizable yet.
  optimize_info = icing.GetOptimizeInfo();
  EXPECT_THAT(optimize_info.status(), ProtoIsOk());
  EXPECT_THAT(optimize_info.optimizable_docs(), Eq(0));
  EXPECT_THAT(optimize_info.estimated_optimizable_bytes(), Eq(0));

  // Deletes document1
  ASSERT_THAT(icing.Delete("namespace", "uri1").status(), ProtoIsOk());

  optimize_info = icing.GetOptimizeInfo();
  EXPECT_THAT(optimize_info.status(), ProtoIsOk());
  EXPECT_THAT(optimize_info.optimizable_docs(), Eq(1));
  EXPECT_THAT(optimize_info.estimated_optimizable_bytes(), Gt(0));
  int64_t first_estimated_optimizable_bytes =
      optimize_info.estimated_optimizable_bytes();

  // Add a second document, but it'll be expired since the time (1000) is
  // greater than the document's creation timestamp (100) + the document's ttl
  // (500)
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  optimize_info = icing.GetOptimizeInfo();
  EXPECT_THAT(optimize_info.status(), ProtoIsOk());
  EXPECT_THAT(optimize_info.optimizable_docs(), Eq(2));
  EXPECT_THAT(optimize_info.estimated_optimizable_bytes(),
              Gt(first_estimated_optimizable_bytes));

  // Optimize
  ASSERT_THAT(icing.Optimize().status(), ProtoIsOk());

  // Nothing is optimizable now that everything has been optimized away.
  optimize_info = icing.GetOptimizeInfo();
  EXPECT_THAT(optimize_info.status(), ProtoIsOk());
  EXPECT_THAT(optimize_info.optimizable_docs(), Eq(0));
  EXPECT_THAT(optimize_info.estimated_optimizable_bytes(), Eq(0));
}

TEST_F(IcingSearchEngineTest, GetAndPutShouldWorkAfterOptimization) {
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");
  DocumentProto document3 = CreateMessageDocument("namespace", "uri3");

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
    ASSERT_THAT(icing.Optimize().status(), ProtoIsOk());

    // Validates that Get() and Put() are good right after Optimize()
    EXPECT_THAT(icing.Get("namespace", "uri1"),
                EqualsProto(expected_get_result_proto));
    EXPECT_THAT(icing.Put(document2).status(), ProtoIsOk());
  }  // Destroys IcingSearchEngine to make sure nothing is cached.

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.Get("namespace", "uri1"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace", "uri2"),
              EqualsProto(expected_get_result_proto));

  EXPECT_THAT(icing.Put(document3).status(), ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, DeleteShouldWorkAfterOptimization) {
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");
  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
    ASSERT_THAT(icing.Optimize().status(), ProtoIsOk());

    // Validates that Delete() works right after Optimize()
    EXPECT_THAT(icing.Delete("namespace", "uri1").status(), ProtoIsOk());

    GetResultProto expected_get_result_proto;
    expected_get_result_proto.mutable_status()->set_code(
        StatusProto::NOT_FOUND);
    expected_get_result_proto.mutable_status()->set_message(
        "Document (namespace, uri1) not found.");
    EXPECT_THAT(icing.Get("namespace", "uri1"),
                EqualsProto(expected_get_result_proto));

    expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
    expected_get_result_proto.mutable_status()->clear_message();
    *expected_get_result_proto.mutable_document() = document2;
    EXPECT_THAT(icing.Get("namespace", "uri2"),
                EqualsProto(expected_get_result_proto));
  }  // Destroys IcingSearchEngine to make sure nothing is cached.

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.Delete("namespace", "uri2").status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace, uri1) not found.");
  EXPECT_THAT(icing.Get("namespace", "uri1"),
              EqualsProto(expected_get_result_proto));

  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace, uri2) not found.");
  EXPECT_THAT(icing.Get("namespace", "uri2"),
              EqualsProto(expected_get_result_proto));
}

TEST_F(IcingSearchEngineTest, OptimizationFailureUninitializesIcing) {
  // Setup filesystem to fail
  auto mock_filesystem = std::make_unique<MockFilesystem>();
  bool just_swapped_files = false;
  auto create_dir_lambda = [this, &just_swapped_files](const char* dir_name) {
    if (just_swapped_files) {
      // We should fail the first call immediately after swapping files.
      just_swapped_files = false;
      return false;
    }
    return filesystem()->CreateDirectoryRecursively(dir_name);
  };
  ON_CALL(*mock_filesystem, CreateDirectoryRecursively)
      .WillByDefault(create_dir_lambda);
  auto swap_lambda = [&just_swapped_files](const char* first_dir,
                                           const char* second_dir) {
    just_swapped_files = true;
    return false;
  };
  ON_CALL(*mock_filesystem, SwapFiles).WillByDefault(swap_lambda);
  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  // The mocks should cause an unrecoverable error during Optimize - returning
  // INTERNAL.
  ASSERT_THAT(icing.Optimize().status(), ProtoStatusIs(StatusProto::INTERNAL));

  // Ordinary operations should fail safely.
  SchemaProto simple_schema;
  auto type = simple_schema.add_types();
  type->set_schema_type("type0");
  auto property = type->add_properties();
  property->set_property_name("prop0");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  DocumentProto simple_doc = DocumentBuilder()
                                 .SetKey("namespace0", "uri0")
                                 .SetSchema("type0")
                                 .AddStringProperty("prop0", "foo")
                                 .Build();

  SearchSpecProto search_spec;
  search_spec.set_query("foo");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
  ResultSpecProto result_spec;
  ScoringSpecProto scoring_spec;
  scoring_spec.set_rank_by(
      ScoringSpecProto::RankingStrategy::CREATION_TIMESTAMP);

  EXPECT_THAT(icing.SetSchema(simple_schema).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.Put(simple_doc).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.Get(simple_doc.namespace_(), simple_doc.uri()).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.Search(search_spec, scoring_spec, result_spec).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));

  // Reset should get icing back to a safe (empty) and working state.
  EXPECT_THAT(icing.Reset().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(simple_schema).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(simple_doc).status(), ProtoIsOk());
  EXPECT_THAT(icing.Get(simple_doc.namespace_(), simple_doc.uri()).status(),
              ProtoIsOk());
  EXPECT_THAT(icing.Search(search_spec, scoring_spec, result_spec).status(),
              ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, DeleteBySchemaType) {
  SchemaProto schema;
  // Add an email type
  auto type = schema.add_types();
  type->set_schema_type("email");
  auto property = type->add_properties();
  property->set_property_name("subject");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
  property->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::EXACT_ONLY);
  property->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);
  // Add an message type
  type = schema.add_types();
  type->set_schema_type("message");
  property = type->add_properties();
  property->set_property_name("body");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
  property->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::EXACT_ONLY);
  property->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace1", "uri1")
          .SetSchema("message")
          .AddStringProperty("body", "message body1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace2", "uri2")
          .SetSchema("email")
          .AddStringProperty("subject", "message body2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  // Delete the first type. The first doc should be irretrievable. The
  // second should still be present.
  EXPECT_THAT(icing.DeleteBySchemaType("message").status(), ProtoIsOk());

  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace1, uri1) not found.");
  expected_get_result_proto.clear_document();
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  expected_get_result_proto.mutable_status()->clear_message();
  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  // Search for "message", only document2 should show up.
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
  search_spec.set_query("message");
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, DeleteSchemaTypeByQuery) {
  SchemaProto schema = CreateMessageSchema();
  // Add an email type
  SchemaProto tmp = CreateEmailSchema();
  *schema.add_types() = tmp.types(0);

  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace1", "uri1")
          .SetSchema(schema.types(0).schema_type())
          .AddStringProperty("body", "message body1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace2", "uri2")
          .SetSchema(schema.types(1).schema_type())
          .AddStringProperty("subject", "subject subject2")
          .AddStringProperty("body", "message body2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document1).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document2).status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  // Delete the first type. The first doc should be irretrievable. The
  // second should still be present.
  SearchSpecProto search_spec;
  search_spec.add_schema_type_filters(schema.types(0).schema_type());
  EXPECT_THAT(icing.DeleteByQuery(search_spec).status(), ProtoIsOk());

  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace1, uri1) not found.");
  expected_get_result_proto.clear_document();
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  expected_get_result_proto.mutable_status()->clear_message();
  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  search_spec = SearchSpecProto::default_instance();
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, DeleteByNamespace) {
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace1", "uri1")
          .SetSchema("Message")
          .AddStringProperty("body", "message body1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace1", "uri2")
          .SetSchema("Message")
          .AddStringProperty("body", "message body2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document3 =
      DocumentBuilder()
          .SetKey("namespace3", "uri3")
          .SetSchema("Message")
          .AddStringProperty("body", "message body2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace1", "uri2"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document3;
  EXPECT_THAT(icing.Get("namespace3", "uri3"),
              EqualsProto(expected_get_result_proto));

  // Delete namespace1. Document1 and document2 should be irretrievable.
  // Document3 should still be present.
  EXPECT_THAT(icing.DeleteByNamespace("namespace1").status(), ProtoIsOk());

  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace1, uri1) not found.");
  expected_get_result_proto.clear_document();
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace1, uri2) not found.");
  expected_get_result_proto.clear_document();
  EXPECT_THAT(icing.Get("namespace1", "uri2"),
              EqualsProto(expected_get_result_proto));

  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  expected_get_result_proto.mutable_status()->clear_message();
  *expected_get_result_proto.mutable_document() = document3;
  EXPECT_THAT(icing.Get("namespace3", "uri3"),
              EqualsProto(expected_get_result_proto));

  // Search for "message", only document3 should show up.
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
  search_spec.set_query("message");
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, DeleteNamespaceByQuery) {
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace1", "uri1")
          .SetSchema("Message")
          .AddStringProperty("body", "message body1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace2", "uri2")
          .SetSchema("Message")
          .AddStringProperty("body", "message body2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document1).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document2).status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  // Delete the first namespace. The first doc should be irretrievable. The
  // second should still be present.
  SearchSpecProto search_spec;
  search_spec.add_namespace_filters("namespace1");
  EXPECT_THAT(icing.DeleteByQuery(search_spec).status(), ProtoIsOk());

  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace1, uri1) not found.");
  expected_get_result_proto.clear_document();
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  expected_get_result_proto.mutable_status()->clear_message();
  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  search_spec = SearchSpecProto::default_instance();
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, DeleteByQuery) {
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace1", "uri1")
          .SetSchema("Message")
          .AddStringProperty("body", "message body1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace2", "uri2")
          .SetSchema("Message")
          .AddStringProperty("body", "message body2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document1).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document2).status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  // Delete all docs containing 'body1'. The first doc should be irretrievable.
  // The second should still be present.
  SearchSpecProto search_spec;
  search_spec.set_query("body1");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
  EXPECT_THAT(icing.DeleteByQuery(search_spec).status(), ProtoIsOk());

  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace1, uri1) not found.");
  expected_get_result_proto.clear_document();
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  expected_get_result_proto.mutable_status()->clear_message();
  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  search_spec = SearchSpecProto::default_instance();
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, DeleteByQueryNotFound) {
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace1", "uri1")
          .SetSchema("Message")
          .AddStringProperty("body", "message body1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace2", "uri2")
          .SetSchema("Message")
          .AddStringProperty("body", "message body2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document1).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document2).status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  // Delete all docs containing 'foo', which should be none of them. Both docs
  // should still be present.
  SearchSpecProto search_spec;
  search_spec.set_query("foo");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
  EXPECT_THAT(icing.DeleteByQuery(search_spec).status(),
              ProtoStatusIs(StatusProto::NOT_FOUND));

  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  expected_get_result_proto.mutable_status()->clear_message();
  *expected_get_result_proto.mutable_document() = document1;
  EXPECT_THAT(icing.Get("namespace1", "uri1"),
              EqualsProto(expected_get_result_proto));

  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  expected_get_result_proto.mutable_status()->clear_message();
  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace2", "uri2"),
              EqualsProto(expected_get_result_proto));

  search_spec = SearchSpecProto::default_instance();
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SetSchemaShouldWorkAfterOptimization) {
  // Creates 3 test schemas
  SchemaProto schema1 = SchemaProto(CreateMessageSchema());

  SchemaProto schema2 = SchemaProto(schema1);
  auto new_property2 = schema2.mutable_types(0)->add_properties();
  new_property2->set_property_name("property2");
  new_property2->set_data_type(PropertyConfigProto::DataType::STRING);
  new_property2->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
  new_property2->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  new_property2->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);

  SchemaProto schema3 = SchemaProto(schema2);
  auto new_property3 = schema3.mutable_types(0)->add_properties();
  new_property3->set_property_name("property3");
  new_property3->set_data_type(PropertyConfigProto::DataType::STRING);
  new_property3->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
  new_property3->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  new_property3->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(schema1).status(), ProtoIsOk());
    ASSERT_THAT(icing.Optimize().status(), ProtoIsOk());

    // Validates that SetSchema() works right after Optimize()
    EXPECT_THAT(icing.SetSchema(schema2).status(), ProtoIsOk());
  }  // Destroys IcingSearchEngine to make sure nothing is cached.

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(schema3).status(), ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, SearchShouldWorkAfterOptimization) {
  DocumentProto document = CreateMessageDocument("namespace", "uri");
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document;

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());
    ASSERT_THAT(icing.Optimize().status(), ProtoIsOk());

    // Validates that Search() works right after Optimize()
    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));
  }  // Destroys IcingSearchEngine to make sure nothing is cached.

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, IcingShouldWorkFineIfOptimizationIsAborted) {
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  {
    // Initializes a normal icing to create files needed
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  }

  // Creates a mock filesystem in which DeleteDirectoryRecursively() always
  // fails. This will fail IcingSearchEngine::OptimizeDocumentStore() and makes
  // it return ABORTED_ERROR.
  auto mock_filesystem = std::make_unique<MockFilesystem>();
  ON_CALL(*mock_filesystem, DeleteDirectoryRecursively)
      .WillByDefault(Return(false));

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.Optimize().status(), ProtoStatusIs(StatusProto::ABORTED));

  // Now optimization is aborted, we verify that document-related functions
  // still work as expected.

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;
  EXPECT_THAT(icing.Get("namespace", "uri1"),
              EqualsProto(expected_get_result_proto));

  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");

  EXPECT_THAT(icing.Put(document2).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_query("m");
  search_spec.set_term_match_type(TermMatchType::PREFIX);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;

  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest,
       OptimizationShouldRecoverIfFileDirectoriesAreMissing) {
  // Creates a mock filesystem in which SwapFiles() always fails and deletes the
  // directories. This will fail IcingSearchEngine::OptimizeDocumentStore().
  auto mock_filesystem = std::make_unique<MockFilesystem>();
  ON_CALL(*mock_filesystem, SwapFiles)
      .WillByDefault([this](const char* one, const char* two) {
        filesystem()->DeleteDirectoryRecursively(one);
        filesystem()->DeleteDirectoryRecursively(two);
        return false;
      });

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());

  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
              ProtoIsOk());

  // Optimize() fails due to filesystem error
  EXPECT_THAT(icing.Optimize().status(),
              ProtoStatusIs(StatusProto::WARNING_DATA_LOSS));

  // Document is not found because original file directory is missing
  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace, uri) not found.");
  EXPECT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));

  DocumentProto new_document =
      DocumentBuilder()
          .SetKey("namespace", "uri2")
          .SetSchema("Message")
          .AddStringProperty("body", "new body")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  EXPECT_THAT(icing.Put(new_document).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_query("m");
  search_spec.set_term_match_type(TermMatchType::PREFIX);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);

  // Searching old content returns nothing because original file directory is
  // missing
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  search_spec.set_query("n");

  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      new_document;

  // Searching new content returns the new document
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, OptimizationShouldRecoverIfDataFilesAreMissing) {
  // Creates a mock filesystem in which SwapFiles() always fails and empties the
  // directories. This will fail IcingSearchEngine::OptimizeDocumentStore().
  auto mock_filesystem = std::make_unique<MockFilesystem>();
  ON_CALL(*mock_filesystem, SwapFiles)
      .WillByDefault([this](const char* one, const char* two) {
        filesystem()->DeleteDirectoryRecursively(one);
        filesystem()->CreateDirectoryRecursively(one);
        filesystem()->DeleteDirectoryRecursively(two);
        filesystem()->CreateDirectoryRecursively(two);
        return false;
      });

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());

  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
              ProtoIsOk());

  // Optimize() fails due to filesystem error
  EXPECT_THAT(icing.Optimize().status(),
              ProtoStatusIs(StatusProto::WARNING_DATA_LOSS));

  // Document is not found because original files are missing
  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace, uri) not found.");
  EXPECT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));

  DocumentProto new_document =
      DocumentBuilder()
          .SetKey("namespace", "uri2")
          .SetSchema("Message")
          .AddStringProperty("body", "new body")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  EXPECT_THAT(icing.Put(new_document).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_query("m");
  search_spec.set_term_match_type(TermMatchType::PREFIX);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);

  // Searching old content returns nothing because original files are missing
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  search_spec.set_query("n");

  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      new_document;

  // Searching new content returns the new document
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchIncludesDocumentsBeforeTtl) {
  SchemaProto schema;
  auto type = schema.add_types();
  type->set_schema_type("Message");

  auto body = type->add_properties();
  body->set_property_name("body");
  body->set_data_type(PropertyConfigProto::DataType::STRING);
  body->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
  body->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  body->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);

  DocumentProto document = DocumentBuilder()
                               .SetKey("namespace", "uri")
                               .SetSchema("Message")
                               .AddStringProperty("body", "message body")
                               .SetCreationTimestampMs(100)
                               .SetTtlMs(500)
                               .Build();

  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document;

  // Time just has to be less than the document's creation timestamp (100) + the
  // document's ttl (500)
  auto fake_clock = std::make_unique<FakeClock>();
  fake_clock->SetSystemTimeMilliseconds(400);

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::make_unique<Filesystem>(),
                              std::make_unique<IcingFilesystem>(),
                              std::move(fake_clock), GetTestJniCache());

  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());

  // Check that the document is returned as part of search results
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchDoesntIncludeDocumentsPastTtl) {
  SchemaProto schema;
  auto type = schema.add_types();
  type->set_schema_type("Message");

  auto body = type->add_properties();
  body->set_property_name("body");
  body->set_data_type(PropertyConfigProto::DataType::STRING);
  body->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
  body->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  body->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);

  DocumentProto document = DocumentBuilder()
                               .SetKey("namespace", "uri")
                               .SetSchema("Message")
                               .AddStringProperty("body", "message body")
                               .SetCreationTimestampMs(100)
                               .SetTtlMs(500)
                               .Build();

  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);

  // Time just has to be greater than the document's creation timestamp (100) +
  // the document's ttl (500)
  auto fake_clock = std::make_unique<FakeClock>();
  fake_clock->SetSystemTimeMilliseconds(700);

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::make_unique<Filesystem>(),
                              std::make_unique<IcingFilesystem>(),
                              std::move(fake_clock), GetTestJniCache());

  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());

  // Check that the document is not returned as part of search results
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchWorksAfterSchemaTypesCompatiblyModified) {
  SchemaProto schema;
  auto type_config = schema.add_types();
  type_config->set_schema_type("message");

  auto property = type_config->add_properties();
  property->set_property_name("body");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  DocumentProto message_document =
      DocumentBuilder()
          .SetKey("namespace", "message_uri")
          .SetSchema("message")
          .AddStringProperty("body", "foo")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(message_document).status(), ProtoIsOk());

  // Make sure we can search for message document
  SearchSpecProto search_spec;
  search_spec.set_query("foo");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);

  // The message isn't indexed, so we get nothing
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  // With just the schema type filter, we can search for the message
  search_spec.Clear();
  search_spec.add_schema_type_filters("message");

  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      message_document;

  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  // Since SchemaTypeIds are assigned based on order in the SchemaProto, this
  // will force a change in the DocumentStore's cached SchemaTypeIds
  schema.clear_types();
  type_config = schema.add_types();
  type_config->set_schema_type("email");

  // Adding a new indexed property will require reindexing
  type_config = schema.add_types();
  type_config->set_schema_type("message");

  property = type_config->add_properties();
  property->set_property_name("body");
  property->set_data_type(PropertyConfigProto::DataType::STRING);
  property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
  property->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  property->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);

  EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());

  search_spec.Clear();
  search_spec.set_query("foo");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
  search_spec.add_schema_type_filters("message");

  // We can still search for the message document
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, RecoverFromMissingHeaderFile) {
  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      CreateMessageDocument("namespace", "uri");

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() =
      CreateMessageDocument("namespace", "uri");

  {
    // Basic initialization/setup
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());
    EXPECT_THAT(icing.Get("namespace", "uri"),
                EqualsProto(expected_get_result_proto));
    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  EXPECT_TRUE(filesystem()->DeleteFile(GetHeaderFilename().c_str()));

  // We should be able to recover from this and access all our previous data
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

  // Checks that DocumentLog is still ok
  EXPECT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));

  // Checks that the index is still ok so we can search over it
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  // Checks that Schema is still since it'll be needed to validate the document
  EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
              ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, RecoverFromInvalidHeaderMagic) {
  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      CreateMessageDocument("namespace", "uri");

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() =
      CreateMessageDocument("namespace", "uri");

  {
    // Basic initialization/setup
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());
    EXPECT_THAT(icing.Get("namespace", "uri"),
                EqualsProto(expected_get_result_proto));
    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  // Change the header's magic value
  int32_t invalid_magic = 1;  // Anything that's not the actual kMagic value.
  filesystem()->PWrite(GetHeaderFilename().c_str(),
                       offsetof(IcingSearchEngine::Header, magic),
                       &invalid_magic, sizeof(invalid_magic));

  // We should be able to recover from this and access all our previous data
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

  // Checks that DocumentLog is still ok
  EXPECT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));

  // Checks that the index is still ok so we can search over it
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  // Checks that Schema is still since it'll be needed to validate the document
  EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
              ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, RecoverFromInvalidHeaderChecksum) {
  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      CreateMessageDocument("namespace", "uri");

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() =
      CreateMessageDocument("namespace", "uri");

  {
    // Basic initialization/setup
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());
    EXPECT_THAT(icing.Get("namespace", "uri"),
                EqualsProto(expected_get_result_proto));
    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  // Change the header's checksum value
  uint32_t invalid_checksum =
      1;  // Anything that's not the actual checksum value
  filesystem()->PWrite(GetHeaderFilename().c_str(),
                       offsetof(IcingSearchEngine::Header, checksum),
                       &invalid_checksum, sizeof(invalid_checksum));

  // We should be able to recover from this and access all our previous data
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

  // Checks that DocumentLog is still ok
  EXPECT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));

  // Checks that the index is still ok so we can search over it
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));

  // Checks that Schema is still since it'll be needed to validate the document
  EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
              ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, UnableToRecoverFromCorruptSchema) {
  {
    // Basic initialization/setup
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());

    GetResultProto expected_get_result_proto;
    expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
    *expected_get_result_proto.mutable_document() =
        CreateMessageDocument("namespace", "uri");

    EXPECT_THAT(icing.Get("namespace", "uri"),
                EqualsProto(expected_get_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  const std::string schema_file =
      absl_ports::StrCat(GetSchemaDir(), "/schema.pb");
  const std::string corrupt_data = "1234";
  EXPECT_TRUE(filesystem()->Write(schema_file.c_str(), corrupt_data.data(),
                                  corrupt_data.size()));

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INTERNAL));
}

TEST_F(IcingSearchEngineTest, UnableToRecoverFromCorruptDocumentLog) {
  {
    // Basic initialization/setup
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());

    GetResultProto expected_get_result_proto;
    expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
    *expected_get_result_proto.mutable_document() =
        CreateMessageDocument("namespace", "uri");

    EXPECT_THAT(icing.Get("namespace", "uri"),
                EqualsProto(expected_get_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  const std::string document_log_file =
      absl_ports::StrCat(GetDocumentDir(), "/document_log");
  const std::string corrupt_data = "1234";
  EXPECT_TRUE(filesystem()->Write(document_log_file.c_str(),
                                  corrupt_data.data(), corrupt_data.size()));

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(),
              ProtoStatusIs(StatusProto::INTERNAL));
}

TEST_F(IcingSearchEngineTest, RecoverFromInconsistentSchemaStore) {
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2_with_additional_property =
      DocumentBuilder()
          .SetKey("namespace", "uri2")
          .SetSchema("Message")
          .AddStringProperty("additional", "content")
          .AddStringProperty("body", "message body")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  {
    // Initializes folder and schema
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

    SchemaProto schema;
    auto type = schema.add_types();
    type->set_schema_type("Message");

    auto property = type->add_properties();
    property->set_property_name("body");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
    property->mutable_string_indexing_config()->set_term_match_type(
        TermMatchType::PREFIX);
    property->mutable_string_indexing_config()->set_tokenizer_type(
        StringIndexingConfig::TokenizerType::PLAIN);

    property = type->add_properties();
    property->set_property_name("additional");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

    EXPECT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(document1).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(document2_with_additional_property).status(),
                ProtoIsOk());

    // Won't get us anything because "additional" isn't marked as an indexed
    // property in the schema
    SearchSpecProto search_spec;
    search_spec.set_query("additional:content");
    search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

    SearchResultProto expected_search_result_proto;
    expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  {
    // This schema will change the SchemaTypeIds from the previous schema_
    // (since SchemaTypeIds are assigned based on order of the types, and this
    // new schema changes the ordering of previous types)
    SchemaProto new_schema;
    auto type = new_schema.add_types();
    type->set_schema_type("Email");

    type = new_schema.add_types();
    type->set_schema_type("Message");

    // Adding a new property changes the SectionIds (since SectionIds are
    // assigned based on alphabetical order of indexed sections, marking
    // "additional" as an indexed property will push the "body" property to a
    // different SectionId)
    auto property = type->add_properties();
    property->set_property_name("body");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
    property->mutable_string_indexing_config()->set_term_match_type(
        TermMatchType::PREFIX);
    property->mutable_string_indexing_config()->set_tokenizer_type(
        StringIndexingConfig::TokenizerType::PLAIN);

    property = type->add_properties();
    property->set_property_name("additional");
    property->set_data_type(PropertyConfigProto::DataType::STRING);
    property->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
    property->mutable_string_indexing_config()->set_term_match_type(
        TermMatchType::PREFIX);
    property->mutable_string_indexing_config()->set_tokenizer_type(
        StringIndexingConfig::TokenizerType::PLAIN);

    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<SchemaStore> schema_store,
        SchemaStore::Create(filesystem(), GetSchemaDir()));
    ICING_EXPECT_OK(schema_store->SetSchema(new_schema));
  }  // Will persist new schema

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

  // We can insert a Email document since we kept the new schema
  DocumentProto email_document =
      DocumentBuilder()
          .SetKey("namespace", "email_uri")
          .SetSchema("Email")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  EXPECT_THAT(icing.Put(email_document).status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = email_document;

  EXPECT_THAT(icing.Get("namespace", "email_uri"),
              EqualsProto(expected_get_result_proto));

  SearchSpecProto search_spec;

  // The section restrict will ensure we are using the correct, updated
  // SectionId in the Index
  search_spec.set_query("additional:content");

  // Schema type filter will ensure we're using the correct, updated
  // SchemaTypeId in the DocumentStore
  search_spec.add_schema_type_filters("Message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2_with_additional_property;

  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, RecoverFromInconsistentDocumentStore) {
  DocumentProto document1 = CreateMessageDocument("namespace", "uri1");
  DocumentProto document2 = CreateMessageDocument("namespace", "uri2");

  {
    // Initializes folder and schema, index one document
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(document1).status(), ProtoIsOk());
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<SchemaStore> schema_store,
        SchemaStore::Create(filesystem(), GetSchemaDir()));
    ICING_EXPECT_OK(schema_store->SetSchema(CreateMessageSchema()));

    // Puts a second document into DocumentStore but doesn't index it.
    FakeClock fake_clock;
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<DocumentStore> document_store,
        DocumentStore::Create(filesystem(), GetDocumentDir(), &fake_clock,
                              schema_store.get()));
    ICING_EXPECT_OK(document_store->Put(document2));
  }

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  // Index Restoration should be triggered here and document2 should be
  // indexed.
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document1;

  // DocumentStore kept the additional document
  EXPECT_THAT(icing.Get("namespace", "uri1"),
              EqualsProto(expected_get_result_proto));

  *expected_get_result_proto.mutable_document() = document2;
  EXPECT_THAT(icing.Get("namespace", "uri2"),
              EqualsProto(expected_get_result_proto));

  // We indexed the additional document
  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;

  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;

  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, RecoverFromInconsistentIndex) {
  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      CreateMessageDocument("namespace", "uri");

  {
    // Initializes folder and schema, index one document
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());
    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  // Pretend we lost the entire index
  EXPECT_TRUE(filesystem()->DeleteDirectoryRecursively(
      absl_ports::StrCat(GetIndexDir(), "/idx/lite.").c_str()));

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

  // Check that our index is ok by searching over the restored index
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, RecoverFromCorruptIndex) {
  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      CreateMessageDocument("namespace", "uri");

  {
    // Initializes folder and schema, index one document
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());
    EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  // Pretend index is corrupted
  const std::string index_hit_buffer_file = GetIndexDir() + "/idx/lite.hb";
  ScopedFd fd(filesystem()->OpenForWrite(index_hit_buffer_file.c_str()));
  ASSERT_TRUE(fd.is_valid());
  ASSERT_TRUE(filesystem()->Write(fd.get(), "1234", 4));

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());

  // Check that our index is ok by searching over the restored index
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchResultShouldBeRankedByDocumentScore) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 documents and ensures the relationship in terms of document
  // score is: document1 < document2 < document3
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace", "uri/1")
          .SetSchema("Message")
          .AddStringProperty("body", "message1")
          .SetScore(1)
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace", "uri/2")
          .SetSchema("Message")
          .AddStringProperty("body", "message2")
          .SetScore(2)
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document3 =
      DocumentBuilder()
          .SetKey("namespace", "uri/3")
          .SetSchema("Message")
          .AddStringProperty("body", "message3")
          .SetScore(3)
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  // Intentionally inserts the documents in the order that is different than
  // their score order
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());

  // "m" will match all 3 documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  // Result should be in descending score order
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;

  ScoringSpecProto scoring_spec = GetDefaultScoringSpec();
  scoring_spec.set_rank_by(ScoringSpecProto::RankingStrategy::DOCUMENT_SCORE);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchShouldAllowNoScoring) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 documents and ensures the relationship of them is:
  // document1 < document2 < document3
  DocumentProto document1 = DocumentBuilder()
                                .SetKey("namespace", "uri/1")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message1")
                                .SetScore(1)
                                .SetCreationTimestampMs(1571111111111)
                                .Build();
  DocumentProto document2 = DocumentBuilder()
                                .SetKey("namespace", "uri/2")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message2")
                                .SetScore(2)
                                .SetCreationTimestampMs(1572222222222)
                                .Build();
  DocumentProto document3 = DocumentBuilder()
                                .SetKey("namespace", "uri/3")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message3")
                                .SetScore(3)
                                .SetCreationTimestampMs(1573333333333)
                                .Build();

  // Intentionally inserts the documents in the order that is different than
  // their score order
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  // "m" will match all 3 documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;

  // Results should not be ranked by score but returned in reverse insertion
  // order.
  ScoringSpecProto scoring_spec = GetDefaultScoringSpec();
  scoring_spec.set_rank_by(ScoringSpecProto::RankingStrategy::NONE);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchResultShouldBeRankedByCreationTimestamp) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 documents and ensures the relationship in terms of creation
  // timestamp score is: document1 < document2 < document3
  DocumentProto document1 = DocumentBuilder()
                                .SetKey("namespace", "uri/1")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message1")
                                .SetCreationTimestampMs(1571111111111)
                                .Build();
  DocumentProto document2 = DocumentBuilder()
                                .SetKey("namespace", "uri/2")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message2")
                                .SetCreationTimestampMs(1572222222222)
                                .Build();
  DocumentProto document3 = DocumentBuilder()
                                .SetKey("namespace", "uri/3")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message3")
                                .SetCreationTimestampMs(1573333333333)
                                .Build();

  // Intentionally inserts the documents in the order that is different than
  // their score order
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  // "m" will match all 3 documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  // Result should be in descending timestamp order
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;

  ScoringSpecProto scoring_spec = GetDefaultScoringSpec();
  scoring_spec.set_rank_by(
      ScoringSpecProto::RankingStrategy::CREATION_TIMESTAMP);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchResultShouldBeRankedByUsageCount) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 test documents
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace", "uri/1")
          .SetSchema("Message")
          .AddStringProperty("body", "message1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace", "uri/2")
          .SetSchema("Message")
          .AddStringProperty("body", "message2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document3 =
      DocumentBuilder()
          .SetKey("namespace", "uri/3")
          .SetSchema("Message")
          .AddStringProperty("body", "message3")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  // Intentionally inserts the documents in a different order to eliminate the
  // possibility that the following results are sorted in the default reverse
  // insertion order.
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  // Report usage for doc3 twice and doc2 once. The order will be doc3 > doc2 >
  // doc1 when ranked by USAGE_TYPE1_COUNT.
  UsageReport usage_report_doc3 = CreateUsageReport(
      /*name_space=*/"namespace", /*uri=*/"uri/3", /*timestamp_ms=*/0,
      UsageReport::USAGE_TYPE1);
  UsageReport usage_report_doc2 = CreateUsageReport(
      /*name_space=*/"namespace", /*uri=*/"uri/2", /*timestamp_ms=*/0,
      UsageReport::USAGE_TYPE1);
  ASSERT_THAT(icing.ReportUsage(usage_report_doc3).status(), ProtoIsOk());
  ASSERT_THAT(icing.ReportUsage(usage_report_doc3).status(), ProtoIsOk());
  ASSERT_THAT(icing.ReportUsage(usage_report_doc2).status(), ProtoIsOk());

  // "m" will match all 3 documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  // Result should be in descending USAGE_TYPE1_COUNT order
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;

  ScoringSpecProto scoring_spec;
  scoring_spec.set_rank_by(
      ScoringSpecProto::RankingStrategy::USAGE_TYPE1_COUNT);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest,
       SearchResultShouldHaveDefaultOrderWithoutUsageCounts) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 test documents
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace", "uri/1")
          .SetSchema("Message")
          .AddStringProperty("body", "message1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace", "uri/2")
          .SetSchema("Message")
          .AddStringProperty("body", "message2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document3 =
      DocumentBuilder()
          .SetKey("namespace", "uri/3")
          .SetSchema("Message")
          .AddStringProperty("body", "message3")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());

  // "m" will match all 3 documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  // None of the documents have usage reports. Result should be in the default
  // reverse insertion order.
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;

  ScoringSpecProto scoring_spec;
  scoring_spec.set_rank_by(
      ScoringSpecProto::RankingStrategy::USAGE_TYPE1_COUNT);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchResultShouldBeRankedByUsageTimestamp) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 test documents
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace", "uri/1")
          .SetSchema("Message")
          .AddStringProperty("body", "message1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace", "uri/2")
          .SetSchema("Message")
          .AddStringProperty("body", "message2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document3 =
      DocumentBuilder()
          .SetKey("namespace", "uri/3")
          .SetSchema("Message")
          .AddStringProperty("body", "message3")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  // Intentionally inserts the documents in a different order to eliminate the
  // possibility that the following results are sorted in the default reverse
  // insertion order.
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  // Report usage for doc2 and doc3. The order will be doc3 > doc2 > doc1 when
  // ranked by USAGE_TYPE1_LAST_USED_TIMESTAMP.
  UsageReport usage_report_doc2 = CreateUsageReport(
      /*name_space=*/"namespace", /*uri=*/"uri/2", /*timestamp_ms=*/1000,
      UsageReport::USAGE_TYPE1);
  UsageReport usage_report_doc3 = CreateUsageReport(
      /*name_space=*/"namespace", /*uri=*/"uri/3", /*timestamp_ms=*/5000,
      UsageReport::USAGE_TYPE1);
  ASSERT_THAT(icing.ReportUsage(usage_report_doc2).status(), ProtoIsOk());
  ASSERT_THAT(icing.ReportUsage(usage_report_doc3).status(), ProtoIsOk());

  // "m" will match all 3 documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  // Result should be in descending USAGE_TYPE1_LAST_USED_TIMESTAMP order
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;

  ScoringSpecProto scoring_spec;
  scoring_spec.set_rank_by(
      ScoringSpecProto::RankingStrategy::USAGE_TYPE1_LAST_USED_TIMESTAMP);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest,
       SearchResultShouldHaveDefaultOrderWithoutUsageTimestamp) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 test documents
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace", "uri/1")
          .SetSchema("Message")
          .AddStringProperty("body", "message1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace", "uri/2")
          .SetSchema("Message")
          .AddStringProperty("body", "message2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document3 =
      DocumentBuilder()
          .SetKey("namespace", "uri/3")
          .SetSchema("Message")
          .AddStringProperty("body", "message3")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());

  // "m" will match all 3 documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  // None of the documents have usage reports. Result should be in the default
  // reverse insertion order.
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;

  ScoringSpecProto scoring_spec;
  scoring_spec.set_rank_by(
      ScoringSpecProto::RankingStrategy::USAGE_TYPE1_LAST_USED_TIMESTAMP);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, OlderUsageTimestampShouldNotOverrideNewerOnes) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 test documents
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace", "uri/1")
          .SetSchema("Message")
          .AddStringProperty("body", "message1")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace", "uri/2")
          .SetSchema("Message")
          .AddStringProperty("body", "message2")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document3 =
      DocumentBuilder()
          .SetKey("namespace", "uri/3")
          .SetSchema("Message")
          .AddStringProperty("body", "message3")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());

  // Report usage for doc1 and doc2. The older timestamp 5000 shouldn't be
  // overridden by 1000. The order will be doc1 > doc2 when ranked by
  // USAGE_TYPE1_LAST_USED_TIMESTAMP.
  UsageReport usage_report_doc1_time1 = CreateUsageReport(
      /*name_space=*/"namespace", /*uri=*/"uri/1", /*timestamp_ms=*/1000,
      UsageReport::USAGE_TYPE1);
  UsageReport usage_report_doc1_time5 = CreateUsageReport(
      /*name_space=*/"namespace", /*uri=*/"uri/1", /*timestamp_ms=*/5000,
      UsageReport::USAGE_TYPE1);
  UsageReport usage_report_doc2_time3 = CreateUsageReport(
      /*name_space=*/"namespace", /*uri=*/"uri/2", /*timestamp_ms=*/3000,
      UsageReport::USAGE_TYPE1);
  ASSERT_THAT(icing.ReportUsage(usage_report_doc1_time5).status(), ProtoIsOk());
  ASSERT_THAT(icing.ReportUsage(usage_report_doc2_time3).status(), ProtoIsOk());
  ASSERT_THAT(icing.ReportUsage(usage_report_doc1_time1).status(), ProtoIsOk());

  // "m" will match both documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  // Result should be in descending USAGE_TYPE1_LAST_USED_TIMESTAMP order
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;

  ScoringSpecProto scoring_spec;
  scoring_spec.set_rank_by(
      ScoringSpecProto::RankingStrategy::USAGE_TYPE1_LAST_USED_TIMESTAMP);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest, SearchResultShouldBeRankedAscendingly) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  // Creates 3 documents and ensures the relationship in terms of document
  // score is: document1 < document2 < document3
  DocumentProto document1 =
      DocumentBuilder()
          .SetKey("namespace", "uri/1")
          .SetSchema("Message")
          .AddStringProperty("body", "message1")
          .SetScore(1)
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document2 =
      DocumentBuilder()
          .SetKey("namespace", "uri/2")
          .SetSchema("Message")
          .AddStringProperty("body", "message2")
          .SetScore(2)
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  DocumentProto document3 =
      DocumentBuilder()
          .SetKey("namespace", "uri/3")
          .SetSchema("Message")
          .AddStringProperty("body", "message3")
          .SetScore(3)
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();

  // Intentionally inserts the documents in the order that is different than
  // their score order
  ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document3).status(), ProtoIsOk());
  ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());

  // "m" will match all 3 documents
  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("m");

  // Result should be in ascending score order
  SearchResultProto expected_search_result_proto;
  expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document1;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document2;
  *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
      document3;

  ScoringSpecProto scoring_spec = GetDefaultScoringSpec();
  scoring_spec.set_rank_by(ScoringSpecProto::RankingStrategy::DOCUMENT_SCORE);
  scoring_spec.set_order_by(ScoringSpecProto::Order::ASC);
  EXPECT_THAT(icing.Search(search_spec, scoring_spec,
                           ResultSpecProto::default_instance()),
              EqualsProto(expected_search_result_proto));
}

TEST_F(IcingSearchEngineTest,
       SetSchemaCanNotDetectPreviousSchemaWasLostWithoutDocuments) {
  SchemaProto schema;
  auto type = schema.add_types();
  type->set_schema_type("Message");

  auto body = type->add_properties();
  body->set_property_name("body");
  body->set_data_type(PropertyConfigProto::DataType::STRING);
  body->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);

  // Make an incompatible schema, a previously OPTIONAL field is REQUIRED
  SchemaProto incompatible_schema = schema;
  incompatible_schema.mutable_types(0)->mutable_properties(0)->set_cardinality(
      PropertyConfigProto::Cardinality::REQUIRED);

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  ASSERT_TRUE(filesystem()->DeleteDirectoryRecursively(GetSchemaDir().c_str()));

  // Since we don't have any documents yet, we can't detect this edge-case.  But
  // it should be fine since there aren't any documents to be invalidated.
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(incompatible_schema).status(), ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, SetSchemaCanDetectPreviousSchemaWasLost) {
  SchemaProto schema;
  auto type = schema.add_types();
  type->set_schema_type("Message");

  auto body = type->add_properties();
  body->set_property_name("body");
  body->set_data_type(PropertyConfigProto::DataType::STRING);
  body->set_cardinality(PropertyConfigProto::Cardinality::OPTIONAL);
  body->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::PREFIX);
  body->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);

  // Make an incompatible schema, a previously OPTIONAL field is REQUIRED
  SchemaProto incompatible_schema = schema;
  incompatible_schema.mutable_types(0)->mutable_properties(0)->set_cardinality(
      PropertyConfigProto::Cardinality::REQUIRED);

  SearchSpecProto search_spec;
  search_spec.set_query("message");
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());

    DocumentProto document = CreateMessageDocument("namespace", "uri");
    ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());

    // Can retrieve by namespace/uri
    GetResultProto expected_get_result_proto;
    expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
    *expected_get_result_proto.mutable_document() = document;

    ASSERT_THAT(icing.Get("namespace", "uri"),
                EqualsProto(expected_get_result_proto));

    // Can search for it
    SearchResultProto expected_search_result_proto;
    expected_search_result_proto.mutable_status()->set_code(StatusProto::OK);
    *expected_search_result_proto.mutable_results()->Add()->mutable_document() =
        CreateMessageDocument("namespace", "uri");
    ASSERT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                             ResultSpecProto::default_instance()),
                EqualsProto(expected_search_result_proto));
  }  // This should shut down IcingSearchEngine and persist anything it needs to

  ASSERT_TRUE(filesystem()->DeleteDirectoryRecursively(GetSchemaDir().c_str()));

  // Setting the new, different schema will remove incompatible documents
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.SetSchema(incompatible_schema).status(), ProtoIsOk());

  // Can't retrieve by namespace/uri
  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::NOT_FOUND);
  expected_get_result_proto.mutable_status()->set_message(
      "Document (namespace, uri) not found.");

  EXPECT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));

  // Can't search for it
  SearchResultProto empty_result;
  empty_result.mutable_status()->set_code(StatusProto::OK);
  EXPECT_THAT(icing.Search(search_spec, GetDefaultScoringSpec(),
                           ResultSpecProto::default_instance()),
              EqualsProto(empty_result));
}

TEST_F(IcingSearchEngineTest, PersistToDisk) {
  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() =
      CreateMessageDocument("namespace", "uri");

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
    EXPECT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
                ProtoIsOk());

    // Persisting shouldn't affect anything
    EXPECT_THAT(icing.PersistToDisk().status(), ProtoIsOk());

    EXPECT_THAT(icing.Get("namespace", "uri"),
                EqualsProto(expected_get_result_proto));
  }  // Destructing persists as well

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  EXPECT_THAT(icing.Initialize().status(), ProtoIsOk());
  EXPECT_THAT(icing.Get("namespace", "uri"),
              EqualsProto(expected_get_result_proto));
}

TEST_F(IcingSearchEngineTest, ResetOk) {
  SchemaProto message_schema = CreateMessageSchema();
  SchemaProto empty_schema = SchemaProto(message_schema);
  empty_schema.clear_types();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(message_schema).status(), ProtoIsOk());

  int64_t empty_state_size =
      filesystem()->GetFileDiskUsage(GetTestBaseDir().c_str());

  DocumentProto document = CreateMessageDocument("namespace", "uri");
  ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());

  // Check that things have been added
  EXPECT_THAT(filesystem()->GetDiskUsage(GetTestBaseDir().c_str()),
              Gt(empty_state_size));

  EXPECT_THAT(icing.Reset().status(), ProtoIsOk());

  // Check that we're back to an empty state
  EXPECT_EQ(filesystem()->GetFileDiskUsage(GetTestBaseDir().c_str()),
            empty_state_size);

  // Sanity check that we can still call other APIs. If things aren't cleared,
  // then this should raise an error since the empty schema is incompatible with
  // the old message_schema.
  EXPECT_THAT(icing.SetSchema(empty_schema).status(), ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, ResetAbortedError) {
  auto mock_filesystem = std::make_unique<MockFilesystem>();

  // This fails IcingSearchEngine::Reset(). But since we didn't actually delete
  // anything, we'll be able to consider this just an ABORTED call.
  ON_CALL(*mock_filesystem,
          DeleteDirectoryRecursively(StrEq(GetTestBaseDir().c_str())))
      .WillByDefault(Return(false));

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document = CreateMessageDocument("namespace", "uri");
  ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());
  EXPECT_THAT(icing.Reset().status(), ProtoStatusIs(StatusProto::ABORTED));

  // Everything is still intact.
  // Can get old data.
  GetResultProto expected_get_result_proto;
  expected_get_result_proto.mutable_status()->set_code(StatusProto::OK);
  *expected_get_result_proto.mutable_document() = document;
  EXPECT_THAT(icing.Get(document.namespace_(), document.uri()),
              EqualsProto(expected_get_result_proto));

  // Can add new data.
  EXPECT_THAT(icing.Put(CreateMessageDocument("namespace", "uri")).status(),
              ProtoIsOk());
}

TEST_F(IcingSearchEngineTest, ResetInternalError) {
  auto mock_filesystem = std::make_unique<MockFilesystem>();

  // Let all other calls succeed.
  EXPECT_CALL(*mock_filesystem, Write(Matcher<const char*>(_), _, _))
      .WillRepeatedly(Return(true));

  // This prevents IcingSearchEngine from creating a DocumentStore instance on
  // reinitialization
  const std::string document_log_path =
      GetTestBaseDir() + "/document_dir/document_log";
  EXPECT_CALL(
      *mock_filesystem,
      Write(Matcher<const char*>(StrEq(document_log_path.c_str())), _, _))
      .WillOnce(Return(true))
      .WillOnce(Return(false));

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  EXPECT_THAT(icing.Reset().status(), ProtoStatusIs(StatusProto::INTERNAL));
}

TEST_F(IcingSearchEngineTest, SnippetNormalization) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document_one =
      DocumentBuilder()
          .SetKey("namespace", "uri1")
          .SetSchema("Message")
          .AddStringProperty("body", "MDI zurich Team Meeting")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  ASSERT_THAT(icing.Put(document_one).status(), ProtoIsOk());

  DocumentProto document_two =
      DocumentBuilder()
          .SetKey("namespace", "uri2")
          .SetSchema("Message")
          .AddStringProperty("body", "mdi Zürich Team Meeting")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  ASSERT_THAT(icing.Put(document_two).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
  search_spec.set_query("mdi Zürich");

  ResultSpecProto result_spec;
  result_spec.mutable_snippet_spec()->set_max_window_bytes(64);
  result_spec.mutable_snippet_spec()->set_num_matches_per_property(2);
  result_spec.mutable_snippet_spec()->set_num_to_snippet(2);

  SearchResultProto results =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  EXPECT_THAT(results.status(), ProtoIsOk());
  ASSERT_THAT(results.results(), SizeIs(2));
  const DocumentProto& result_document_1 = results.results(0).document();
  const SnippetProto& result_snippet_1 = results.results(0).snippet();
  EXPECT_THAT(result_document_1, EqualsProto(document_two));
  EXPECT_THAT(GetMatch(result_document_1, result_snippet_1, "body",
                       /*snippet_index=*/0),
              Eq("mdi"));
  EXPECT_THAT(GetWindow(result_document_1, result_snippet_1, "body",
                        /*snippet_index=*/0),
              Eq("mdi Zürich Team Meeting"));
  EXPECT_THAT(GetMatch(result_document_1, result_snippet_1, "body",
                       /*snippet_index=*/1),
              Eq("Zürich"));
  EXPECT_THAT(GetWindow(result_document_1, result_snippet_1, "body",
                        /*snippet_index=*/1),
              Eq("mdi Zürich Team Meeting"));

  const DocumentProto& result_document_2 = results.results(1).document();
  const SnippetProto& result_snippet_2 = results.results(1).snippet();
  EXPECT_THAT(result_document_2, EqualsProto(document_one));
  EXPECT_THAT(GetMatch(result_document_2, result_snippet_2, "body",
                       /*snippet_index=*/0),
              Eq("MDI"));
  EXPECT_THAT(GetWindow(result_document_2, result_snippet_2, "body",
                        /*snippet_index=*/0),
              Eq("MDI zurich Team Meeting"));
  EXPECT_THAT(GetMatch(result_document_2, result_snippet_2, "body",
                       /*snippet_index=*/1),
              Eq("zurich"));
  EXPECT_THAT(GetWindow(result_document_2, result_snippet_2, "body",
                        /*snippet_index=*/1),
              Eq("MDI zurich Team Meeting"));
}

TEST_F(IcingSearchEngineTest, SnippetNormalizationPrefix) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  DocumentProto document_one =
      DocumentBuilder()
          .SetKey("namespace", "uri1")
          .SetSchema("Message")
          .AddStringProperty("body", "MDI zurich Team Meeting")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  ASSERT_THAT(icing.Put(document_one).status(), ProtoIsOk());

  DocumentProto document_two =
      DocumentBuilder()
          .SetKey("namespace", "uri2")
          .SetSchema("Message")
          .AddStringProperty("body", "mdi Zürich Team Meeting")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  ASSERT_THAT(icing.Put(document_two).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("md Zür");

  ResultSpecProto result_spec;
  result_spec.mutable_snippet_spec()->set_max_window_bytes(64);
  result_spec.mutable_snippet_spec()->set_num_matches_per_property(2);
  result_spec.mutable_snippet_spec()->set_num_to_snippet(2);

  SearchResultProto results =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  EXPECT_THAT(results.status(), ProtoIsOk());
  ASSERT_THAT(results.results(), SizeIs(2));
  const DocumentProto& result_document_1 = results.results(0).document();
  const SnippetProto& result_snippet_1 = results.results(0).snippet();
  EXPECT_THAT(result_document_1, EqualsProto(document_two));
  EXPECT_THAT(GetMatch(result_document_1, result_snippet_1, "body",
                       /*snippet_index=*/0),
              Eq("mdi"));
  EXPECT_THAT(GetWindow(result_document_1, result_snippet_1, "body",
                        /*snippet_index=*/0),
              Eq("mdi Zürich Team Meeting"));
  EXPECT_THAT(GetMatch(result_document_1, result_snippet_1, "body",
                       /*snippet_index=*/1),
              Eq("Zürich"));
  EXPECT_THAT(GetWindow(result_document_1, result_snippet_1, "body",
                        /*snippet_index=*/1),
              Eq("mdi Zürich Team Meeting"));

  const DocumentProto& result_document_2 = results.results(1).document();
  const SnippetProto& result_snippet_2 = results.results(1).snippet();
  EXPECT_THAT(result_document_2, EqualsProto(document_one));
  EXPECT_THAT(GetMatch(result_document_2, result_snippet_2, "body",
                       /*snippet_index=*/0),
              Eq("MDI"));
  EXPECT_THAT(GetWindow(result_document_2, result_snippet_2, "body",
                        /*snippet_index=*/0),
              Eq("MDI zurich Team Meeting"));
  EXPECT_THAT(GetMatch(result_document_2, result_snippet_2, "body",
                       /*snippet_index=*/1),
              Eq("zurich"));
  EXPECT_THAT(GetWindow(result_document_2, result_snippet_2, "body",
                        /*snippet_index=*/1),
              Eq("MDI zurich Team Meeting"));
}

TEST_F(IcingSearchEngineTest, SnippetSectionRestrict) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateEmailSchema()).status(), ProtoIsOk());

  DocumentProto document_one =
      DocumentBuilder()
          .SetKey("namespace", "uri1")
          .SetSchema("Email")
          .AddStringProperty("subject", "MDI zurich Team Meeting")
          .AddStringProperty("body", "MDI zurich Team Meeting")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  ASSERT_THAT(icing.Put(document_one).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::PREFIX);
  search_spec.set_query("body:Zür");

  ResultSpecProto result_spec;
  result_spec.mutable_snippet_spec()->set_max_window_bytes(64);
  result_spec.mutable_snippet_spec()->set_num_matches_per_property(10);
  result_spec.mutable_snippet_spec()->set_num_to_snippet(10);

  SearchResultProto results =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);
  EXPECT_THAT(results.status(), ProtoIsOk());
  ASSERT_THAT(results.results(), SizeIs(1));
  const DocumentProto& result_document = results.results(0).document();
  const SnippetProto& result_snippet = results.results(0).snippet();
  EXPECT_THAT(result_document, EqualsProto(document_one));
  EXPECT_THAT(
      GetMatch(result_document, result_snippet, "body", /*snippet_index=*/0),
      Eq("zurich"));
  EXPECT_THAT(
      GetWindow(result_document, result_snippet, "body", /*snippet_index=*/0),
      Eq("MDI zurich Team Meeting"));
  EXPECT_THAT(
      GetMatch(result_document, result_snippet, "subject", /*snippet_index=*/0),
      IsEmpty());
  EXPECT_THAT(GetWindow(result_document, result_snippet, "subject",
                        /*snippet_index=*/0),
              IsEmpty());
}

TEST_F(IcingSearchEngineTest, UninitializedInstanceFailsSafely) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());

  SchemaProto email_schema = CreateMessageSchema();
  EXPECT_THAT(icing.SetSchema(email_schema).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.GetSchema().status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.GetSchemaType(email_schema.types(0).schema_type()).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));

  DocumentProto doc = CreateMessageDocument("namespace", "uri");
  EXPECT_THAT(icing.Put(doc).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.Get(doc.namespace_(), doc.uri()).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.Delete(doc.namespace_(), doc.uri()).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.DeleteByNamespace(doc.namespace_()).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.DeleteBySchemaType(email_schema.types(0).schema_type())
                  .status()
                  .code(),
              Eq(StatusProto::FAILED_PRECONDITION));

  SearchSpecProto search_spec = SearchSpecProto::default_instance();
  ScoringSpecProto scoring_spec = ScoringSpecProto::default_instance();
  ResultSpecProto result_spec = ResultSpecProto::default_instance();
  EXPECT_THAT(icing.Search(search_spec, scoring_spec, result_spec).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  constexpr int kSomePageToken = 12;
  EXPECT_THAT(icing.GetNextPage(kSomePageToken).status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  icing.InvalidateNextPageToken(kSomePageToken);  // Verify this doesn't crash.

  EXPECT_THAT(icing.PersistToDisk().status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
  EXPECT_THAT(icing.Optimize().status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
}

TEST_F(IcingSearchEngineTest, GetAllNamespaces) {
  DocumentProto namespace1 = DocumentBuilder()
                                 .SetKey("namespace1", "uri")
                                 .SetSchema("Message")
                                 .AddStringProperty("body", "message body")
                                 .SetCreationTimestampMs(100)
                                 .SetTtlMs(1000)
                                 .Build();
  DocumentProto namespace2_uri1 = DocumentBuilder()
                                      .SetKey("namespace2", "uri1")
                                      .SetSchema("Message")
                                      .AddStringProperty("body", "message body")
                                      .SetCreationTimestampMs(100)
                                      .SetTtlMs(1000)
                                      .Build();
  DocumentProto namespace2_uri2 = DocumentBuilder()
                                      .SetKey("namespace2", "uri2")
                                      .SetSchema("Message")
                                      .AddStringProperty("body", "message body")
                                      .SetCreationTimestampMs(100)
                                      .SetTtlMs(1000)
                                      .Build();

  DocumentProto namespace3 = DocumentBuilder()
                                 .SetKey("namespace3", "uri")
                                 .SetSchema("Message")
                                 .AddStringProperty("body", "message body")
                                 .SetCreationTimestampMs(100)
                                 .SetTtlMs(500)
                                 .Build();
  {
    // Some arbitrary time that's less than all the document's creation time +
    // ttl
    auto fake_clock = std::make_unique<FakeClock>();
    fake_clock->SetSystemTimeMilliseconds(500);

    TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                                std::make_unique<Filesystem>(),
                                std::make_unique<IcingFilesystem>(),
                                std::move(fake_clock), GetTestJniCache());

    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    // No namespaces exist yet
    GetAllNamespacesResultProto result = icing.GetAllNamespaces();
    EXPECT_THAT(result.status(), ProtoIsOk());
    EXPECT_THAT(result.namespaces(), IsEmpty());

    ASSERT_THAT(icing.Put(namespace1).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(namespace2_uri1).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(namespace2_uri2).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(namespace3).status(), ProtoIsOk());

    // All namespaces should exist now
    result = icing.GetAllNamespaces();
    EXPECT_THAT(result.status(), ProtoIsOk());
    EXPECT_THAT(result.namespaces(),
                UnorderedElementsAre("namespace1", "namespace2", "namespace3"));

    // After deleting namespace2_uri1 document, we still have namespace2_uri2 in
    // "namespace2" so it should still show up
    ASSERT_THAT(icing.Delete("namespace2", "uri1").status(), ProtoIsOk());

    result = icing.GetAllNamespaces();
    EXPECT_THAT(result.status(), ProtoIsOk());
    EXPECT_THAT(result.namespaces(),
                UnorderedElementsAre("namespace1", "namespace2", "namespace3"));

    // After deleting namespace2_uri2 document, we no longer have any documents
    // in "namespace2"
    ASSERT_THAT(icing.Delete("namespace2", "uri2").status(), ProtoIsOk());

    result = icing.GetAllNamespaces();
    EXPECT_THAT(result.status(), ProtoIsOk());
    EXPECT_THAT(result.namespaces(),
                UnorderedElementsAre("namespace1", "namespace3"));
  }

  // We reinitialize here so we can feed in a fake clock this time
  {
    // Time needs to be past namespace3's creation time (100) + ttl (500) for it
    // to count as "expired"
    auto fake_clock = std::make_unique<FakeClock>();
    fake_clock->SetSystemTimeMilliseconds(1000);

    TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                                std::make_unique<Filesystem>(),
                                std::make_unique<IcingFilesystem>(),
                                std::move(fake_clock), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    // Only valid document left is the one in "namespace1"
    GetAllNamespacesResultProto result = icing.GetAllNamespaces();
    EXPECT_THAT(result.status(), ProtoIsOk());
    EXPECT_THAT(result.namespaces(), UnorderedElementsAre("namespace1"));
  }
}

TEST_F(IcingSearchEngineTest, Hyphens) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

  SchemaProto schema;
  SchemaTypeConfigProto* type = schema.add_types();
  type->set_schema_type("MyType");
  PropertyConfigProto* prop = type->add_properties();
  prop->set_property_name("foo");
  prop->set_data_type(PropertyConfigProto::DataType::STRING);
  prop->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
  prop->mutable_string_indexing_config()->set_term_match_type(
      TermMatchType::EXACT_ONLY);
  prop->mutable_string_indexing_config()->set_tokenizer_type(
      StringIndexingConfig::TokenizerType::PLAIN);
  ASSERT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());

  DocumentProto document_one =
      DocumentBuilder()
          .SetKey("namespace", "uri1")
          .SetSchema("MyType")
          .AddStringProperty("foo", "foo bar-baz bat")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  ASSERT_THAT(icing.Put(document_one).status(), ProtoIsOk());

  DocumentProto document_two =
      DocumentBuilder()
          .SetKey("namespace", "uri2")
          .SetSchema("MyType")
          .AddStringProperty("foo", "bar for baz bat-man")
          .SetCreationTimestampMs(kDefaultCreationTimestampMs)
          .Build();
  ASSERT_THAT(icing.Put(document_two).status(), ProtoIsOk());

  SearchSpecProto search_spec;
  search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
  search_spec.set_query("foo:bar-baz");

  ResultSpecProto result_spec;
  SearchResultProto results =
      icing.Search(search_spec, GetDefaultScoringSpec(), result_spec);

  EXPECT_THAT(results.status(), ProtoIsOk());
  ASSERT_THAT(results.results(), SizeIs(2));
  EXPECT_THAT(results.results(0).document(), EqualsProto(document_two));
  EXPECT_THAT(results.results(1).document(), EqualsProto(document_one));
}

TEST_F(IcingSearchEngineTest, RestoreIndex) {
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", kIpsumText)
                               .Build();
  // 1. Create an index with a LiteIndex that will only allow one document
  // before needing a merge.
  {
    IcingSearchEngineOptions options = GetDefaultIcingOptions();
    options.set_index_merge_size(document.ByteSizeLong());
    IcingSearchEngine icing(options, GetTestJniCache());

    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    // Add two documents. These should get merged into the main index.
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
    document = DocumentBuilder(document).SetUri("fake_type/1").Build();
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
    // Add one document. This one should get remain in the lite index.
    document = DocumentBuilder(document).SetUri("fake_type/2").Build();
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
  }

  // 2. Delete the index file to trigger RestoreIndexIfNeeded.
  std::string idx_subdir = GetIndexDir() + "/idx";
  filesystem()->DeleteDirectoryRecursively(idx_subdir.c_str());

  // 3. Create the index again. This should trigger index restoration.
  {
    IcingSearchEngineOptions options = GetDefaultIcingOptions();
    options.set_index_merge_size(document.ByteSizeLong());
    IcingSearchEngine icing(options, GetTestJniCache());

    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    SearchSpecProto search_spec;
    search_spec.set_query("consectetur");
    search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
    SearchResultProto results =
        icing.Search(search_spec, ScoringSpecProto::default_instance(),
                     ResultSpecProto::default_instance());
    EXPECT_THAT(results.status(), ProtoIsOk());
    EXPECT_THAT(results.next_page_token(), Eq(0));
    // All documents should be retrievable.
    ASSERT_THAT(results.results(), SizeIs(3));
    EXPECT_THAT(results.results(0).document().uri(), Eq("fake_type/2"));
    EXPECT_THAT(results.results(1).document().uri(), Eq("fake_type/1"));
    EXPECT_THAT(results.results(2).document().uri(), Eq("fake_type/0"));
  }
}

TEST_F(IcingSearchEngineTest, RestoreIndexLoseLiteIndex) {
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", kIpsumText)
                               .Build();
  // 1. Create an index with a LiteIndex that will only allow one document
  // before needing a merge.
  {
    IcingSearchEngineOptions options = GetDefaultIcingOptions();
    options.set_index_merge_size(document.ByteSizeLong());
    IcingSearchEngine icing(options, GetTestJniCache());

    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    // Add two documents. These should get merged into the main index.
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
    document = DocumentBuilder(document).SetUri("fake_type/1").Build();
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
    // Add one document. This one should get remain in the lite index.
    document = DocumentBuilder(document).SetUri("fake_type/2").Build();
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
  }

  // 2. Delete the last document from the document log
  {
    const std::string document_log_file =
        absl_ports::StrCat(GetDocumentDir(), "/document_log");
    filesystem()->DeleteFile(document_log_file.c_str());
    ICING_ASSERT_OK_AND_ASSIGN(auto create_result,
                               FileBackedProtoLog<DocumentWrapper>::Create(
                                   filesystem(), document_log_file.c_str(),
                                   FileBackedProtoLog<DocumentWrapper>::Options(
                                       /*compress_in=*/true)));
    std::unique_ptr<FileBackedProtoLog<DocumentWrapper>> document_log =
        std::move(create_result.proto_log);

    document = DocumentBuilder(document).SetUri("fake_type/0").Build();
    DocumentWrapper wrapper;
    *wrapper.mutable_document() = document;
    ASSERT_THAT(document_log->WriteProto(wrapper), IsOk());

    document = DocumentBuilder(document).SetUri("fake_type/1").Build();
    *wrapper.mutable_document() = document;
    ASSERT_THAT(document_log->WriteProto(wrapper), IsOk());
  }

  // 3. Create the index again. This should throw out the lite index and trigger
  // index restoration which will only restore the two documents in the main
  // index.
  {
    IcingSearchEngineOptions options = GetDefaultIcingOptions();
    options.set_index_merge_size(document.ByteSizeLong());
    IcingSearchEngine icing(options, GetTestJniCache());

    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    SearchSpecProto search_spec;
    search_spec.set_query("consectetur");
    search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
    SearchResultProto results =
        icing.Search(search_spec, ScoringSpecProto::default_instance(),
                     ResultSpecProto::default_instance());
    EXPECT_THAT(results.status(), ProtoIsOk());
    EXPECT_THAT(results.next_page_token(), Eq(0));
    // Only the documents that were in the main index should be retrievable.
    ASSERT_THAT(results.results(), SizeIs(2));
    EXPECT_THAT(results.results(0).document().uri(), Eq("fake_type/1"));
    EXPECT_THAT(results.results(1).document().uri(), Eq("fake_type/0"));
  }
}

TEST_F(IcingSearchEngineTest, RestoreIndexLoseIndex) {
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", kIpsumText)
                               .Build();
  // 1. Create an index with a LiteIndex that will only allow one document
  // before needing a merge.
  {
    IcingSearchEngineOptions options = GetDefaultIcingOptions();
    options.set_index_merge_size(document.ByteSizeLong());
    IcingSearchEngine icing(options, GetTestJniCache());

    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    // Add two documents. These should get merged into the main index.
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
    document = DocumentBuilder(document).SetUri("fake_type/1").Build();
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
    // Add one document. This one should get remain in the lite index.
    document = DocumentBuilder(document).SetUri("fake_type/2").Build();
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
  }

  // 2. Delete the last two documents from the document log.
  {
    const std::string document_log_file =
        absl_ports::StrCat(GetDocumentDir(), "/document_log");
    filesystem()->DeleteFile(document_log_file.c_str());
    ICING_ASSERT_OK_AND_ASSIGN(auto create_result,
                               FileBackedProtoLog<DocumentWrapper>::Create(
                                   filesystem(), document_log_file.c_str(),
                                   FileBackedProtoLog<DocumentWrapper>::Options(
                                       /*compress_in=*/true)));
    std::unique_ptr<FileBackedProtoLog<DocumentWrapper>> document_log =
        std::move(create_result.proto_log);

    document = DocumentBuilder(document).SetUri("fake_type/0").Build();
    DocumentWrapper wrapper;
    *wrapper.mutable_document() = document;
    ASSERT_THAT(document_log->WriteProto(wrapper), IsOk());
  }

  // 3. Create the index again. This should throw out the lite and main index
  // and trigger index restoration.
  {
    IcingSearchEngineOptions options = GetDefaultIcingOptions();
    options.set_index_merge_size(document.ByteSizeLong());
    IcingSearchEngine icing(options, GetTestJniCache());

    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());

    SearchSpecProto search_spec;
    search_spec.set_query("consectetur");
    search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
    SearchResultProto results =
        icing.Search(search_spec, ScoringSpecProto::default_instance(),
                     ResultSpecProto::default_instance());
    EXPECT_THAT(results.status(), ProtoIsOk());
    EXPECT_THAT(results.next_page_token(), Eq(0));
    // Only the first document should be retrievable.
    ASSERT_THAT(results.results(), SizeIs(1));
    EXPECT_THAT(results.results(0).document().uri(), Eq("fake_type/0"));
  }
}

TEST_F(IcingSearchEngineTest, IndexingDocMergeFailureResets) {
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", kIpsumText)
                               .Build();
  // 1. Create an index with a LiteIndex that will only allow one document
  // before needing a merge.
  {
    IcingSearchEngineOptions options = GetDefaultIcingOptions();
    options.set_index_merge_size(document.ByteSizeLong());
    IcingSearchEngine icing(options, GetTestJniCache());

    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    // Add two documents. These should get merged into the main index.
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
    document = DocumentBuilder(document).SetUri("fake_type/1").Build();
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
    // Add one document. This one should get remain in the lite index.
    document = DocumentBuilder(document).SetUri("fake_type/2").Build();
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
  }

  // 2. Delete the index file to trigger RestoreIndexIfNeeded.
  std::string idx_subdir = GetIndexDir() + "/idx";
  filesystem()->DeleteDirectoryRecursively(idx_subdir.c_str());

  // 3. Setup a mock filesystem to fail to grow the main index once.
  bool has_failed_already = false;
  auto open_write_lambda = [this, &has_failed_already](const char* filename) {
    std::string main_lexicon_suffix = "/main-lexicon.prop.2";
    std::string filename_string(filename);
    if (!has_failed_already &&
        filename_string.length() >= main_lexicon_suffix.length() &&
        filename_string.substr(
            filename_string.length() - main_lexicon_suffix.length(),
            main_lexicon_suffix.length()) == main_lexicon_suffix) {
      has_failed_already = true;
      return -1;
    }
    return this->filesystem()->OpenForWrite(filename);
  };
  auto mock_icing_filesystem = std::make_unique<IcingMockFilesystem>();
  ON_CALL(*mock_icing_filesystem, OpenForWrite)
      .WillByDefault(open_write_lambda);

  // 4. Create the index again. This should trigger index restoration.
  {
    IcingSearchEngineOptions options = GetDefaultIcingOptions();
    options.set_index_merge_size(document.ByteSizeLong());
    TestIcingSearchEngine icing(options, std::make_unique<Filesystem>(),
                                std::move(mock_icing_filesystem),
                                std::make_unique<FakeClock>(),
                                GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(),
                ProtoStatusIs(StatusProto::WARNING_DATA_LOSS));

    SearchSpecProto search_spec;
    search_spec.set_query("consectetur");
    search_spec.set_term_match_type(TermMatchType::EXACT_ONLY);
    SearchResultProto results =
        icing.Search(search_spec, ScoringSpecProto::default_instance(),
                     ResultSpecProto::default_instance());
    EXPECT_THAT(results.status(), ProtoIsOk());
    EXPECT_THAT(results.next_page_token(), Eq(0));
    // Only the last document that was added should still be retrievable.
    ASSERT_THAT(results.results(), SizeIs(1));
    EXPECT_THAT(results.results(0).document().uri(), Eq("fake_type/2"));
  }
}

TEST_F(IcingSearchEngineTest, InitializeShouldLogFunctionLatency) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  InitializeResultProto initialize_result_proto = icing.Initialize();
  EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
  EXPECT_THAT(initialize_result_proto.native_initialize_stats().latency_ms(),
              Gt(0));
}

TEST_F(IcingSearchEngineTest, InitializeShouldLogNumberOfDocuments) {
  DocumentProto document1 = DocumentBuilder()
                                .SetKey("icing", "fake_type/1")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message body")
                                .Build();
  DocumentProto document2 = DocumentBuilder()
                                .SetKey("icing", "fake_type/2")
                                .SetSchema("Message")
                                .AddStringProperty("body", "message body")
                                .Build();

  {
    // Initialize and put a document.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(
        initialize_result_proto.native_initialize_stats().num_documents(),
        Eq(0));

    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    ASSERT_THAT(icing.Put(document1).status(), ProtoIsOk());
  }

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(
        initialize_result_proto.native_initialize_stats().num_documents(),
        Eq(1));

    // Put another document.
    ASSERT_THAT(icing.Put(document2).status(), ProtoIsOk());
  }

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(
        initialize_result_proto.native_initialize_stats().num_documents(),
        Eq(2));
  }
}

TEST_F(IcingSearchEngineTest,
       InitializeShouldNotLogRecoveryCauseForFirstTimeInitialize) {
  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  InitializeResultProto initialize_result_proto = icing.Initialize();
  EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_recovery_cause(),
              Eq(NativeInitializeStats::NONE));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_recovery_latency_ms(),
              Eq(0));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_data_status(),
              Eq(NativeInitializeStats::NO_DATA_LOSS));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .index_restoration_cause(),
              Eq(NativeInitializeStats::NONE));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .index_restoration_latency_ms(),
              Eq(0));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .schema_store_recovery_cause(),
              Eq(NativeInitializeStats::NONE));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .schema_store_recovery_latency_ms(),
              Eq(0));
}

TEST_F(IcingSearchEngineTest, InitializeShouldLogRecoveryCausePartialDataLoss) {
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", "message body")
                               .Build();

  {
    // Initialize and put a document.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
  }

  {
    // Append a non-checksummed document. This will mess up the checksum of the
    // proto log, forcing it to rewind and later return a DATA_LOSS error.
    const std::string serialized_document = document.SerializeAsString();
    const std::string document_log_file =
        absl_ports::StrCat(GetDocumentDir(), "/document_log");

    int64_t file_size = filesystem()->GetFileSize(document_log_file.c_str());
    filesystem()->PWrite(document_log_file.c_str(), file_size,
                         serialized_document.data(),
                         serialized_document.size());
  }

  {
    // Document store will rewind to previous checkpoint. The cause should be
    // DATA_LOSS and the data status should be PARTIAL_LOSS.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_cause(),
                Eq(NativeInitializeStats::DATA_LOSS));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_latency_ms(),
                Gt(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_data_status(),
                Eq(NativeInitializeStats::PARTIAL_LOSS));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_cause(),
                Eq(NativeInitializeStats::NONE));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_latency_ms(),
                Eq(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_cause(),
                Eq(NativeInitializeStats::NONE));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_latency_ms(),
                Eq(0));
  }
}

TEST_F(IcingSearchEngineTest,
       InitializeShouldLogRecoveryCauseCompleteDataLoss) {
  DocumentProto document1 = DocumentBuilder()
                                .SetKey("icing", "fake_type/1")
                                .SetSchema("Message")
                                .AddStringProperty("body", kIpsumText)
                                .Build();
  DocumentProto document2 = DocumentBuilder()
                                .SetKey("icing", "fake_type/2")
                                .SetSchema("Message")
                                .AddStringProperty("body", kIpsumText)
                                .Build();

  {
    // Initialize and put a document.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(document1).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(document2).status(), ProtoIsOk());
  }

  {
    // Modify the document log checksum to trigger a complete document log
    // rewind.
    const std::string document_log_file =
        absl_ports::StrCat(GetDocumentDir(), "/document_log");

    FileBackedProtoLog<DocumentWrapper>::Header document_log_header;
    filesystem()->PRead(document_log_file.c_str(), &document_log_header,
                        sizeof(FileBackedProtoLog<DocumentWrapper>::Header),
                        /*offset=*/0);
    // Set a garbage checksum.
    document_log_header.log_checksum = 10;
    document_log_header.header_checksum =
        document_log_header.CalculateHeaderChecksum();
    filesystem()->PWrite(document_log_file.c_str(), /*offset=*/0,
                         &document_log_header,
                         sizeof(FileBackedProtoLog<DocumentWrapper>::Header));
  }

  {
    // Document store will completely rewind. The cause should be DATA_LOSS and
    // the data status should be COMPLETE_LOSS.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_cause(),
                Eq(NativeInitializeStats::DATA_LOSS));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_latency_ms(),
                Gt(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_data_status(),
                Eq(NativeInitializeStats::COMPLETE_LOSS));
    // The complete rewind of ground truth causes the mismatch of total
    // checksum, so index should be restored.
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_cause(),
                Eq(NativeInitializeStats::TOTAL_CHECKSUM_MISMATCH));
    // Here we don't check index_restoration_latency_ms because the index
    // restoration is super fast when document store is emtpy. We won't get a
    // latency that is greater than 1 ms.
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_cause(),
                Eq(NativeInitializeStats::NONE));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_latency_ms(),
                Eq(0));
  }
}

TEST_F(IcingSearchEngineTest,
       InitializeShouldLogRecoveryCauseInconsistentWithGroundTruth) {
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", "message body")
                               .Build();
  {
    // Initialize and put a document.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
    EXPECT_THAT(icing.Put(document).status(), ProtoIsOk());
  }

  {
    // Delete the index file to trigger RestoreIndexIfNeeded.
    std::string idx_subdir = GetIndexDir() + "/idx";
    filesystem()->DeleteDirectoryRecursively(idx_subdir.c_str());
  }

  {
    // Index is empty but ground truth is not. Index should be restored due to
    // the inconsistency.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_cause(),
                Eq(NativeInitializeStats::INCONSISTENT_WITH_GROUND_TRUTH));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_latency_ms(),
                Gt(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_cause(),
                Eq(NativeInitializeStats::NONE));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_latency_ms(),
                Eq(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_data_status(),
                Eq(NativeInitializeStats::NO_DATA_LOSS));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_cause(),
                Eq(NativeInitializeStats::NONE));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_latency_ms(),
                Eq(0));
  }
}

TEST_F(IcingSearchEngineTest,
       InitializeShouldLogRecoveryCauseTotalChecksumMismatch) {
  {
    // Initialize and index some documents.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    // We need to index enough documents to make
    // DocumentStore::UpdateSchemaStore() run longer than 1 ms.
    for (int i = 0; i < 50; ++i) {
      DocumentProto document =
          DocumentBuilder()
              .SetKey("icing", "fake_type/" + std::to_string(i))
              .SetSchema("Message")
              .AddStringProperty("body", "message body")
              .Build();
      ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());
    }
  }

  {
    // Change the header's checksum value to a random value.
    uint32_t invalid_checksum = 1;
    filesystem()->PWrite(GetHeaderFilename().c_str(),
                         offsetof(IcingSearchEngine::Header, checksum),
                         &invalid_checksum, sizeof(invalid_checksum));
  }

  {
    // Both document store and index should be recovered from checksum mismatch.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_cause(),
                Eq(NativeInitializeStats::TOTAL_CHECKSUM_MISMATCH));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_latency_ms(),
                Gt(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_cause(),
                Eq(NativeInitializeStats::TOTAL_CHECKSUM_MISMATCH));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_latency_ms(),
                Gt(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_data_status(),
                Eq(NativeInitializeStats::NO_DATA_LOSS));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_cause(),
                Eq(NativeInitializeStats::NONE));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_latency_ms(),
                Eq(0));
  }
}

TEST_F(IcingSearchEngineTest, InitializeShouldLogRecoveryCauseIndexIOError) {
  {
    // Initialize and index some documents.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    // We need to index enough documents to make RestoreIndexIfNeeded() run
    // longer than 1 ms.
    for (int i = 0; i < 50; ++i) {
      DocumentProto document =
          DocumentBuilder()
              .SetKey("icing", "fake_type/" + std::to_string(i))
              .SetSchema("Message")
              .AddStringProperty("body", "message body")
              .Build();
      ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());
    }
  }

  // lambda to fail OpenForWrite on lite index hit buffer once.
  bool has_failed_already = false;
  auto open_write_lambda = [this, &has_failed_already](const char* filename) {
    std::string lite_index_buffer_file_path =
        absl_ports::StrCat(GetIndexDir(), "/idx/lite.hb");
    std::string filename_string(filename);
    if (!has_failed_already && filename_string == lite_index_buffer_file_path) {
      has_failed_already = true;
      return -1;
    }
    return this->filesystem()->OpenForWrite(filename);
  };

  auto mock_icing_filesystem = std::make_unique<IcingMockFilesystem>();
  // This fails Index::Create() once.
  ON_CALL(*mock_icing_filesystem, OpenForWrite)
      .WillByDefault(open_write_lambda);

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::make_unique<Filesystem>(),
                              std::move(mock_icing_filesystem),
                              std::make_unique<FakeClock>(), GetTestJniCache());

  InitializeResultProto initialize_result_proto = icing.Initialize();
  EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .index_restoration_cause(),
              Eq(NativeInitializeStats::IO_ERROR));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .index_restoration_latency_ms(),
              Gt(0));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_recovery_cause(),
              Eq(NativeInitializeStats::NONE));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_recovery_latency_ms(),
              Eq(0));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_data_status(),
              Eq(NativeInitializeStats::NO_DATA_LOSS));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .schema_store_recovery_cause(),
              Eq(NativeInitializeStats::NONE));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .schema_store_recovery_latency_ms(),
              Eq(0));
}

TEST_F(IcingSearchEngineTest, InitializeShouldLogRecoveryCauseDocStoreIOError) {
  {
    // Initialize and index some documents.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

    // We need to index enough documents to make RestoreIndexIfNeeded() run
    // longer than 1 ms.
    for (int i = 0; i < 50; ++i) {
      DocumentProto document =
          DocumentBuilder()
              .SetKey("icing", "fake_type/" + std::to_string(i))
              .SetSchema("Message")
              .AddStringProperty("body", "message body")
              .Build();
      ASSERT_THAT(icing.Put(document).status(), ProtoIsOk());
    }
  }

  // lambda to fail Read on document store header once.
  bool has_failed_already = false;
  auto read_lambda = [this, &has_failed_already](const char* filename,
                                                 void* buf, size_t buf_size) {
    std::string document_store_header_file_path =
        absl_ports::StrCat(GetDocumentDir(), "/document_store_header");
    std::string filename_string(filename);
    if (!has_failed_already &&
        filename_string == document_store_header_file_path) {
      has_failed_already = true;
      return false;
    }
    return this->filesystem()->Read(filename, buf, buf_size);
  };

  auto mock_filesystem = std::make_unique<MockFilesystem>();
  // This fails DocumentStore::InitializeDerivedFiles() once.
  ON_CALL(*mock_filesystem, Read(A<const char*>(), _, _))
      .WillByDefault(read_lambda);

  TestIcingSearchEngine icing(GetDefaultIcingOptions(),
                              std::move(mock_filesystem),
                              std::make_unique<IcingFilesystem>(),
                              std::make_unique<FakeClock>(), GetTestJniCache());

  InitializeResultProto initialize_result_proto = icing.Initialize();
  EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_recovery_cause(),
              Eq(NativeInitializeStats::IO_ERROR));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_recovery_latency_ms(),
              Gt(0));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .document_store_data_status(),
              Eq(NativeInitializeStats::NO_DATA_LOSS));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .index_restoration_cause(),
              Eq(NativeInitializeStats::NONE));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .index_restoration_latency_ms(),
              Eq(0));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .schema_store_recovery_cause(),
              Eq(NativeInitializeStats::NONE));
  EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                  .schema_store_recovery_latency_ms(),
              Eq(0));
}

TEST_F(IcingSearchEngineTest,
       InitializeShouldLogRecoveryCauseSchemaStoreIOError) {
  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  }

  {
    // Delete the schema store header file to trigger an I/O error.
    std::string schema_store_header_file_path =
        GetSchemaDir() + "/schema_store_header";
    filesystem()->DeleteFile(schema_store_header_file_path.c_str());
  }

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_cause(),
                Eq(NativeInitializeStats::IO_ERROR));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .schema_store_recovery_latency_ms(),
                Gt(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_cause(),
                Eq(NativeInitializeStats::NONE));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_recovery_latency_ms(),
                Eq(0));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .document_store_data_status(),
                Eq(NativeInitializeStats::NO_DATA_LOSS));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_cause(),
                Eq(NativeInitializeStats::NONE));
    EXPECT_THAT(initialize_result_proto.native_initialize_stats()
                    .index_restoration_latency_ms(),
                Eq(0));
  }
}

TEST_F(IcingSearchEngineTest, InitializeShouldLogNumberOfSchemaTypes) {
  {
    // Initialize an empty storage.
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    // There should be 0 schema types.
    EXPECT_THAT(
        initialize_result_proto.native_initialize_stats().num_schema_types(),
        Eq(0));

    // Set a schema with one type config.
    ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  }

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    // There should be 1 schema type.
    EXPECT_THAT(
        initialize_result_proto.native_initialize_stats().num_schema_types(),
        Eq(1));

    // Create and set a schema with two type configs: Email and Message.
    SchemaProto schema = CreateEmailSchema();

    auto type = schema.add_types();
    type->set_schema_type("Message");
    auto body = type->add_properties();
    body->set_property_name("body");
    body->set_data_type(PropertyConfigProto::DataType::STRING);
    body->set_cardinality(PropertyConfigProto::Cardinality::REQUIRED);
    body->mutable_string_indexing_config()->set_term_match_type(
        TermMatchType::PREFIX);
    body->mutable_string_indexing_config()->set_tokenizer_type(
        StringIndexingConfig::TokenizerType::PLAIN);

    ASSERT_THAT(icing.SetSchema(schema).status(), ProtoIsOk());
  }

  {
    IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
    InitializeResultProto initialize_result_proto = icing.Initialize();
    EXPECT_THAT(initialize_result_proto.status(), ProtoIsOk());
    EXPECT_THAT(
        initialize_result_proto.native_initialize_stats().num_schema_types(),
        Eq(2));
  }
}

TEST_F(IcingSearchEngineTest, PutDocumentShouldLogFunctionLatency) {
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", "message body")
                               .Build();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  PutResultProto put_result_proto = icing.Put(document);
  EXPECT_THAT(put_result_proto.status(), ProtoIsOk());
  EXPECT_THAT(put_result_proto.native_put_document_stats().latency_ms(), Gt(0));
}

TEST_F(IcingSearchEngineTest, PutDocumentShouldLogDocumentStoreStats) {
  // Create a large enough document so that document_store_latency_ms can be
  // longer than 1 ms.
  std::default_random_engine random;
  std::string random_string_10000 =
      RandomString(kAlNumAlphabet, /*len=*/10000, &random);
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", random_string_10000)
                               .Build();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  PutResultProto put_result_proto = icing.Put(document);
  EXPECT_THAT(put_result_proto.status(), ProtoIsOk());
  EXPECT_THAT(
      put_result_proto.native_put_document_stats().document_store_latency_ms(),
      Gt(0));
  EXPECT_THAT(put_result_proto.native_put_document_stats().document_size(),
              Eq(document.ByteSizeLong()));
}

TEST_F(IcingSearchEngineTest, PutDocumentShouldLogIndexingStats) {
  // Create a large enough document so that index_latency_ms can be longer than
  // 1 ms.
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", kIpsumText)
                               .Build();

  IcingSearchEngine icing(GetDefaultIcingOptions(), GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  PutResultProto put_result_proto = icing.Put(document);
  EXPECT_THAT(put_result_proto.status(), ProtoIsOk());
  EXPECT_THAT(put_result_proto.native_put_document_stats().index_latency_ms(),
              Gt(0));
  // No merge should happen.
  EXPECT_THAT(
      put_result_proto.native_put_document_stats().index_merge_latency_ms(),
      Eq(0));
  // Number of tokens should not exceed.
  EXPECT_FALSE(put_result_proto.native_put_document_stats()
                   .tokenization_stats()
                   .exceeded_max_token_num());
  // kIpsumText has 137 tokens.
  EXPECT_THAT(put_result_proto.native_put_document_stats()
                  .tokenization_stats()
                  .num_tokens_indexed(),
              Eq(137));
}

TEST_F(IcingSearchEngineTest, PutDocumentShouldLogWhetherNumTokensExceeds) {
  // Create a document with 2 tokens.
  DocumentProto document = DocumentBuilder()
                               .SetKey("icing", "fake_type/0")
                               .SetSchema("Message")
                               .AddStringProperty("body", "message body")
                               .Build();

  // Create an icing instance with max_tokens_per_doc = 1.
  IcingSearchEngineOptions icing_options = GetDefaultIcingOptions();
  icing_options.set_max_tokens_per_doc(1);
  IcingSearchEngine icing(icing_options, GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());

  PutResultProto put_result_proto = icing.Put(document);
  EXPECT_THAT(put_result_proto.status(), ProtoIsOk());
  // Number of tokens(2) exceeds the max allowed value(1).
  EXPECT_TRUE(put_result_proto.native_put_document_stats()
                  .tokenization_stats()
                  .exceeded_max_token_num());
  EXPECT_THAT(put_result_proto.native_put_document_stats()
                  .tokenization_stats()
                  .num_tokens_indexed(),
              Eq(1));
}

TEST_F(IcingSearchEngineTest, PutDocumentShouldLogIndexMergeLatency) {
  // Create 2 large enough documents so that index_merge_latency_ms can be
  // longer than 1 ms.
  DocumentProto document1 = DocumentBuilder()
                                .SetKey("icing", "fake_type/1")
                                .SetSchema("Message")
                                .AddStringProperty("body", kIpsumText)
                                .Build();
  DocumentProto document2 = DocumentBuilder()
                                .SetKey("icing", "fake_type/2")
                                .SetSchema("Message")
                                .AddStringProperty("body", kIpsumText)
                                .Build();

  // Create an icing instance with index_merge_size = document1's size.
  IcingSearchEngineOptions icing_options = GetDefaultIcingOptions();
  icing_options.set_index_merge_size(document1.ByteSizeLong());
  IcingSearchEngine icing(icing_options, GetTestJniCache());
  ASSERT_THAT(icing.Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing.SetSchema(CreateMessageSchema()).status(), ProtoIsOk());
  EXPECT_THAT(icing.Put(document1).status(), ProtoIsOk());

  // Putting document2 should trigger an index merge.
  PutResultProto put_result_proto = icing.Put(document2);
  EXPECT_THAT(put_result_proto.status(), ProtoIsOk());
  EXPECT_THAT(
      put_result_proto.native_put_document_stats().index_merge_latency_ms(),
      Gt(0));
}

}  // namespace
}  // namespace lib
}  // namespace icing
