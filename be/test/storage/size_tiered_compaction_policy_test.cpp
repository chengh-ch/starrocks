// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "storage/size_tiered_compaction_policy.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <memory>

#include "fs/fs_util.h"
#include "runtime/exec_env.h"
#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "storage/chunk_helper.h"
#include "storage/compaction.h"
#include "storage/compaction_context.h"
#include "storage/compaction_manager.h"
#include "storage/compaction_utils.h"
#include "storage/cumulative_compaction.h"
#include "storage/rowset/rowset_factory.h"
#include "storage/rowset/rowset_writer.h"
#include "storage/rowset/rowset_writer_context.h"
#include "storage/storage_engine.h"
#include "storage/tablet_meta.h"
#include "testutil/assert.h"

namespace starrocks::vectorized {

class SizeTieredCompactionPolicyTest : public testing::Test {
public:
    ~SizeTieredCompactionPolicyTest() override {
        if (_engine) {
            _engine->stop();
            delete _engine;
            _engine = nullptr;
        }
    }

    void rowset_writer_add_rows(std::unique_ptr<RowsetWriter>& writer, int64_t level) {
        std::srand(std::time(nullptr));
        std::vector<std::string> test_data;
        auto schema = ChunkHelper::convert_schema_to_format_v2(*_tablet_schema);
        auto chunk = ChunkHelper::new_chunk(schema, 1024);
        for (size_t i = 0; i < 24576 * pow(config::size_tiered_level_multiple + 1, level - 2); ++i) {
            test_data.push_back("well" + std::to_string(std::rand()));
            auto& cols = chunk->columns();
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(std::rand())));
            Slice field_1(test_data[i]);
            cols[1]->append_datum(vectorized::Datum(field_1));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(10000 + std::rand())));
        }
        CHECK_OK(writer->add_chunk(*chunk));
    }

    void write_new_version(TabletMetaSharedPtr tablet_meta, int64_t level = 2) {
        RowsetWriterContext rowset_writer_context;
        create_rowset_writer_context(&rowset_writer_context, _version);
        _version++;
        std::unique_ptr<RowsetWriter> rowset_writer;
        ASSERT_TRUE(RowsetFactory::create_rowset_writer(rowset_writer_context, &rowset_writer).ok());

        rowset_writer_add_rows(rowset_writer, level);

        rowset_writer->flush();
        RowsetSharedPtr src_rowset = *rowset_writer->build();
        ASSERT_TRUE(src_rowset != nullptr);
        LOG(INFO) << "rowset version " << src_rowset->start_version() << " size " << src_rowset->data_disk_size();

        tablet_meta->add_rs_meta(src_rowset->rowset_meta());
    }

    void write_specify_version(TabletSharedPtr tablet, int64_t version, int64_t level = 2) {
        RowsetWriterContext rowset_writer_context;
        create_rowset_writer_context(&rowset_writer_context, version);
        std::unique_ptr<RowsetWriter> rowset_writer;
        ASSERT_TRUE(RowsetFactory::create_rowset_writer(rowset_writer_context, &rowset_writer).ok());

        rowset_writer_add_rows(rowset_writer, level);

        rowset_writer->flush();
        RowsetSharedPtr src_rowset = *rowset_writer->build();
        ASSERT_TRUE(src_rowset != nullptr);
        LOG(INFO) << "rowset version " << src_rowset->start_version() << " size " << src_rowset->data_disk_size();

        ASSERT_TRUE(tablet->add_rowset(src_rowset).ok());
    }

    void write_delete_version(TabletMetaSharedPtr tablet_meta, int64_t version) {
        RowsetWriterContext rowset_writer_context;
        create_rowset_writer_context(&rowset_writer_context, version);
        std::unique_ptr<RowsetWriter> rowset_writer;
        ASSERT_TRUE(RowsetFactory::create_rowset_writer(rowset_writer_context, &rowset_writer).ok());

        rowset_writer->flush();
        RowsetSharedPtr src_rowset = *rowset_writer->build();
        ASSERT_TRUE(src_rowset != nullptr);
        ASSERT_EQ(0, src_rowset->num_rows());

        auto* delete_predicate = src_rowset->rowset_meta()->mutable_delete_predicate();
        delete_predicate->set_version(version);
        auto* in_pred = delete_predicate->add_in_predicates();
        in_pred->set_column_name("k1");
        in_pred->set_is_not_in(false);
        in_pred->add_values("0");

        tablet_meta->add_rs_meta(src_rowset->rowset_meta());
    }

    void create_rowset_writer_context(RowsetWriterContext* rowset_writer_context, int64_t version) {
        RowsetId rowset_id;
        rowset_id.init(_rowset_id++);
        rowset_writer_context->rowset_id = rowset_id;
        rowset_writer_context->tablet_id = 12345;
        rowset_writer_context->tablet_schema_hash = 1111;
        rowset_writer_context->partition_id = 10;
        rowset_writer_context->rowset_path_prefix = config::storage_root_path + "/data/0/12345/1111";
        rowset_writer_context->rowset_state = VISIBLE;
        rowset_writer_context->tablet_schema = _tablet_schema.get();
        rowset_writer_context->version.first = version;
        rowset_writer_context->version.second = version;
    }

    void create_tablet_schema(KeysType keys_type) {
        TabletSchemaPB tablet_schema_pb;
        tablet_schema_pb.set_keys_type(keys_type);
        tablet_schema_pb.set_num_short_key_columns(2);
        tablet_schema_pb.set_num_rows_per_row_block(1024);
        tablet_schema_pb.set_next_column_unique_id(4);

        ColumnPB* column_1 = tablet_schema_pb.add_column();
        column_1->set_unique_id(1);
        column_1->set_name("k1");
        column_1->set_type("INT");
        column_1->set_is_key(true);
        column_1->set_length(4);
        column_1->set_index_length(4);
        column_1->set_is_nullable(false);
        column_1->set_is_bf_column(false);

        ColumnPB* column_2 = tablet_schema_pb.add_column();
        column_2->set_unique_id(2);
        column_2->set_name("k2");
        column_2->set_type("VARCHAR");
        column_2->set_length(20);
        column_2->set_index_length(20);
        column_2->set_is_key(true);
        column_2->set_is_nullable(false);
        column_2->set_is_bf_column(false);

        ColumnPB* column_3 = tablet_schema_pb.add_column();
        column_3->set_unique_id(3);
        column_3->set_name("v1");
        column_3->set_type("INT");
        column_3->set_length(4);
        column_3->set_is_key(false);
        column_3->set_is_nullable(false);
        column_3->set_is_bf_column(false);
        column_3->set_aggregation("SUM");

        _tablet_schema = std::make_unique<TabletSchema>(tablet_schema_pb);
    }

    void create_tablet_meta(TabletMeta* tablet_meta) {
        TabletMetaPB tablet_meta_pb;
        tablet_meta_pb.set_table_id(10000);
        tablet_meta_pb.set_tablet_id(12345);
        tablet_meta_pb.set_schema_hash(1111);
        tablet_meta_pb.set_partition_id(10);
        tablet_meta_pb.set_shard_id(0);
        tablet_meta_pb.set_creation_time(1575020449);
        tablet_meta_pb.set_tablet_state(PB_RUNNING);
        PUniqueId* tablet_uid = tablet_meta_pb.mutable_tablet_uid();
        tablet_uid->set_hi(10);
        tablet_uid->set_lo(10);

        TabletSchemaPB* tablet_schema_pb = tablet_meta_pb.mutable_schema();
        _tablet_schema->to_schema_pb(tablet_schema_pb);

        tablet_meta->init_from_pb(&tablet_meta_pb);
    }

    void init_compaction_context(TabletSharedPtr tablet) {
        std::unique_ptr<CompactionContext> compaction_context = std::make_unique<CompactionContext>();
        compaction_context->policy = std::make_unique<SizeTieredCompactionPolicy>(tablet.get());
        tablet->set_compaction_context(compaction_context);
    }

    Status compact(TabletSharedPtr tablet) {
        if (!tablet->need_compaction()) {
            LOG(WARNING) << "no need compact";
            return Status::InternalError("no need compact");
        }

        auto task = tablet->create_compaction_task();
        if (task == nullptr) {
            LOG(WARNING) << "task is null";
            return Status::InternalError("task is null");
        }

        task->run();
        if (task->compaction_task_state() == COMPACTION_FAILED) {
            LOG(WARNING) << "task fail";
            return Status::InternalError("task fail");
        }

        return Status::OK();
    }

    void SetUp() override {
        config::min_cumulative_compaction_num_singleton_deltas = 2;
        config::max_cumulative_compaction_num_singleton_deltas = 5;
        config::max_compaction_concurrency = 1;
        config::min_base_compaction_num_singleton_deltas = 10;
        Compaction::init(config::max_compaction_concurrency);

        config::storage_root_path = std::filesystem::current_path().string() + "/data_test_cumulative_compaction";
        fs::remove_all(config::storage_root_path);
        ASSERT_TRUE(fs::create_directories(config::storage_root_path).ok());
        std::vector<StorePath> paths;
        paths.emplace_back(config::storage_root_path);

        starrocks::EngineOptions options;
        options.store_paths = paths;
        options.compaction_mem_tracker = _compaction_mem_tracker.get();
        if (_engine == nullptr) {
            Status s = starrocks::StorageEngine::open(options, &_engine);
            ASSERT_TRUE(s.ok()) << s.to_string();
        }

        _engine->compaction_manager()->init_max_task_num(1);
        _engine->compaction_manager()->_disable_update_tablet = true;

        _schema_hash_path = fmt::format("{}/data/0/12345/1111", config::storage_root_path);
        ASSERT_OK(fs::create_directories(_schema_hash_path));

        _metadata_mem_tracker = std::make_unique<MemTracker>(-1);
        _mem_pool = std::make_unique<MemPool>();

        _compaction_mem_tracker = std::make_unique<MemTracker>(-1);

        _rowset_id = 10000;
        _version = 0;
    }

    void TearDown() override {
        if (fs::path_exist(config::storage_root_path)) {
            ASSERT_TRUE(fs::remove_all(config::storage_root_path).ok());
        }
    }

protected:
    StorageEngine* _engine = nullptr;
    std::unique_ptr<TabletSchema> _tablet_schema;
    std::string _schema_hash_path;
    std::unique_ptr<MemTracker> _metadata_mem_tracker;
    std::unique_ptr<MemTracker> _compaction_mem_tracker;
    std::unique_ptr<MemPool> _mem_pool;

    int64_t _rowset_id;
    int64_t _version;
};

TEST_F(SizeTieredCompactionPolicyTest, test_init_succeeded) {
    TabletMetaSharedPtr tablet_meta(new TabletMeta());
    TabletSharedPtr tablet = Tablet::create_tablet_from_meta(tablet_meta, nullptr);
    init_compaction_context(tablet);
    ASSERT_FALSE(compact(tablet).ok());
}

TEST_F(SizeTieredCompactionPolicyTest, test_candidate_rowsets_empty) {
    TabletSchemaPB schema_pb;
    schema_pb.set_keys_type(KeysType::DUP_KEYS);

    auto schema = std::make_shared<const TabletSchema>(schema_pb);
    TabletMetaSharedPtr tablet_meta(new TabletMeta());
    tablet_meta->set_tablet_schema(schema);

    TabletSharedPtr tablet = Tablet::create_tablet_from_meta(tablet_meta, nullptr);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_FALSE(compact(tablet).ok());
}

TEST_F(SizeTieredCompactionPolicyTest, test_min_compaction) {
    LOG(INFO) << "test_min_cumulative_compaction";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    auto res = compact(tablet);
    ASSERT_FALSE(res.ok());

    ASSERT_EQ(1, tablet->version_count());
    std::vector<Version> versions;
    tablet->list_versions(&versions);
    ASSERT_EQ(1, versions.size());
    ASSERT_EQ(0, versions[0].first);
    ASSERT_EQ(0, versions[0].second);
}

TEST_F(SizeTieredCompactionPolicyTest, test_max_compaction) {
    LOG(INFO) << "test_max_cumulative_compaction";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    for (int i = 0; i < 6; ++i) {
        write_new_version(tablet_meta);
    }

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    auto res = compact(tablet);
    ASSERT_TRUE(res.ok());

    ASSERT_EQ(1, tablet->version_count());
    std::vector<Version> versions;
    tablet->list_versions(&versions);
    ASSERT_EQ(1, versions.size());
    ASSERT_EQ(0, versions[0].first);
    ASSERT_EQ(5, versions[0].second);
}

TEST_F(SizeTieredCompactionPolicyTest, test_missed_first_version) {
    LOG(INFO) << "test_missed_first_version";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta);
    _version++;
    write_new_version(tablet_meta);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    {
        auto res = compact(tablet);
        ASSERT_FALSE(res.ok());

        ASSERT_EQ(2, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(2, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(2, versions[1].first);
        ASSERT_EQ(2, versions[1].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_missed_version_after_cumulative_point) {
    LOG(INFO) << "test_missed_version_after_cumulative_point";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    for (int i = 0; i < 2; ++i) {
        write_new_version(tablet_meta);
    }
    _version++;
    for (int i = 0; i < 2; ++i) {
        write_new_version(tablet_meta);
    }

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(4, tablet->version_count());

    // compaction 3-4
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(1, versions[1].second);
        ASSERT_EQ(3, versions[2].first);
        ASSERT_EQ(4, versions[2].second);
    }

    // compaction 1-2
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(2, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(2, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(1, versions[0].second);
        ASSERT_EQ(3, versions[1].first);
        ASSERT_EQ(4, versions[1].second);
    }

    // write 2
    {
        write_specify_version(tablet, 2);
        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(1, versions[0].second);
        ASSERT_EQ(2, versions[1].first);
        ASSERT_EQ(2, versions[1].second);
        ASSERT_EQ(3, versions[2].first);
        ASSERT_EQ(4, versions[2].second);
    }

    // compaction 2
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(4, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_missed_two_version) {
    LOG(INFO) << "test_missed_two_version";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    for (int i = 0; i < 2; ++i) {
        write_new_version(tablet_meta);
    }
    _version += 2;
    for (int i = 0; i < 2; ++i) {
        write_new_version(tablet_meta);
    }

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(4, tablet->version_count());

    // compaction 4-5
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(1, versions[1].second);
        ASSERT_EQ(4, versions[2].first);
        ASSERT_EQ(5, versions[2].second);
    }

    // compaction 0-1
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(2, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(2, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(1, versions[0].second);
        ASSERT_EQ(4, versions[1].first);
        ASSERT_EQ(5, versions[1].second);
    }

    // write version 2
    {
        write_specify_version(tablet, 2);
        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(1, versions[0].second);
        ASSERT_EQ(2, versions[1].first);
        ASSERT_EQ(2, versions[1].second);
        ASSERT_EQ(4, versions[2].first);
        ASSERT_EQ(5, versions[2].second);
    }

    // compaction 0-2
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(2, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(2, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(2, versions[0].second);
        ASSERT_EQ(4, versions[1].first);
        ASSERT_EQ(5, versions[1].second);
    }

    // write version 3
    {
        write_specify_version(tablet, 3);
        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(2, versions[0].second);
        ASSERT_EQ(3, versions[1].first);
        ASSERT_EQ(3, versions[1].second);
        ASSERT_EQ(4, versions[2].first);
        ASSERT_EQ(5, versions[2].second);
    }

    // compaction 0-5
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(5, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_delete_version) {
    LOG(INFO) << "test_delete_version";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta);
    _version++;
    write_delete_version(tablet_meta, 1);
    write_new_version(tablet_meta);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(3, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(2, versions[0].second);
    }
}

/*
FIXME(meego)
TEST_F(SizeTieredCompactionPolicyTest, test_missed_and_delete_version) {
    LOG(INFO) << "test_missed_delete_version";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    for (int i = 0; i < 2; ++i) {
        write_new_version(tablet_meta);
    }
    _version += 2;
    write_delete_version(tablet_meta, 3);

    _version += 2;
    for (int i = 0; i < 2; ++i) {
        write_new_version(tablet_meta);
    }

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(5, tablet->version_count());

    // compaction 6-7
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(4, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(4, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(1, versions[1].second);
        ASSERT_EQ(3, versions[2].first);
        ASSERT_EQ(3, versions[2].second);
        ASSERT_EQ(6, versions[3].first);
        ASSERT_EQ(7, versions[3].second);
    }

    // compaction 0-1
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(1, versions[0].second);
        ASSERT_EQ(3, versions[1].first);
        ASSERT_EQ(3, versions[1].second);
        ASSERT_EQ(6, versions[2].first);
        ASSERT_EQ(7, versions[2].second);
    }

    // write version 2
    {
        write_specify_version(tablet, 2);
        ASSERT_EQ(4, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(4, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(1, versions[0].second);
        ASSERT_EQ(2, versions[1].first);
        ASSERT_EQ(2, versions[1].second);
        ASSERT_EQ(3, versions[2].first);
        ASSERT_EQ(3, versions[2].second);
        ASSERT_EQ(6, versions[3].first);
        ASSERT_EQ(7, versions[3].second);
    }

    // compaction 0-3
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(2, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(2, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(3, versions[0].second);
        ASSERT_EQ(6, versions[1].first);
        ASSERT_EQ(7, versions[1].second);
    }
}
*/

TEST_F(SizeTieredCompactionPolicyTest, test_two_delete_version) {
    LOG(INFO) << "test_two_delete_version";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta);
    _version++;
    write_delete_version(tablet_meta, 1);
    _version++;
    write_delete_version(tablet_meta, 2);
    write_new_version(tablet_meta);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(4, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(3, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_two_delete_missed_version) {
    LOG(INFO) << "test_two_delete_missed_version";
    create_tablet_schema(UNIQUE_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta);
    _version++;
    _version++;
    write_delete_version(tablet_meta, 2);
    _version++;
    write_delete_version(tablet_meta, 3);
    write_new_version(tablet_meta);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(4, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_FALSE(res.ok());

        ASSERT_EQ(4, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(4, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(2, versions[1].first);
        ASSERT_EQ(2, versions[1].second);
        ASSERT_EQ(3, versions[2].first);
        ASSERT_EQ(3, versions[2].second);
        ASSERT_EQ(4, versions[3].first);
        ASSERT_EQ(4, versions[3].second);
    }

    // write version 1
    {
        write_specify_version(tablet, 1);
        ASSERT_EQ(5, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(5, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(1, versions[1].second);
        ASSERT_EQ(2, versions[2].first);
        ASSERT_EQ(2, versions[2].second);
        ASSERT_EQ(3, versions[3].first);
        ASSERT_EQ(3, versions[3].second);
        ASSERT_EQ(4, versions[4].first);
        ASSERT_EQ(4, versions[4].second);
    }

    // compaction 0-4
    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(4, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_write_descending_order_level_size) {
    LOG(INFO) << "test_write_descending_order_level_size";
    create_tablet_schema(DUP_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta, 4);
    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 2);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(3, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_FALSE(res.ok());

        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(1, versions[1].second);
        ASSERT_EQ(2, versions[2].first);
        ASSERT_EQ(2, versions[2].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_write_order_level_size) {
    LOG(INFO) << "test_write_order_level_size";
    create_tablet_schema(DUP_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta, 2);
    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 4);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(3, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(2, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_write_multi_descending_order_level_size) {
    LOG(INFO) << "test_write_multi_descending_order_level_size";
    create_tablet_schema(DUP_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta, 4);
    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 2);
    write_new_version(tablet_meta, 2);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(5, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(4, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(4, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(1, versions[1].second);
        ASSERT_EQ(2, versions[2].first);
        ASSERT_EQ(2, versions[2].second);
        ASSERT_EQ(3, versions[3].first);
        ASSERT_EQ(4, versions[3].second);
    }

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(2, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(2, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(4, versions[1].second);
    }

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(4, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_backtrace_base_compaction) {
    LOG(INFO) << "test_backtrace_base_compaction";
    create_tablet_schema(DUP_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 2);
    write_delete_version(tablet_meta, 2);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(3, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(2, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_base_and_backtrace_compaction) {
    LOG(INFO) << "test_base_and_backtrace_compaction";
    create_tablet_schema(DUP_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 2);
    write_delete_version(tablet_meta, 3);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(4, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(1, versions[0].second);
        ASSERT_EQ(2, versions[1].first);
        ASSERT_EQ(2, versions[1].second);
        ASSERT_EQ(3, versions[2].first);
        ASSERT_EQ(3, versions[2].second);
    }

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(3, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_backtrace_cumulative_compaction) {
    LOG(INFO) << "test_backtrace_cumulative_compaction";
    create_tablet_schema(DUP_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta, 4);
    write_new_version(tablet_meta, 4);
    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 2);
    write_delete_version(tablet_meta, 4);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(5, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(4, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(4, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(1, versions[1].second);
        ASSERT_EQ(2, versions[2].first);
        ASSERT_EQ(3, versions[2].second);
        ASSERT_EQ(4, versions[3].first);
        ASSERT_EQ(4, versions[3].second);
    }

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(1, versions[0].second);
        ASSERT_EQ(2, versions[1].first);
        ASSERT_EQ(3, versions[1].second);
        ASSERT_EQ(4, versions[2].first);
        ASSERT_EQ(4, versions[2].second);
    }

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(4, versions[0].second);
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_no_backtrace_compaction) {
    LOG(INFO) << "test_no_backtrace_compaction";
    create_tablet_schema(DUP_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta, 3);
    _version++;
    _version++;
    write_delete_version(tablet_meta, 2);
    write_new_version(tablet_meta, 2);
    _version++;
    write_delete_version(tablet_meta, 4);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(4, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_FALSE(res.ok());
        ASSERT_EQ(4, tablet->version_count());
    }
}

TEST_F(SizeTieredCompactionPolicyTest, test_force_base_compaction) {
    LOG(INFO) << "test_force_base_compaction";
    create_tablet_schema(DUP_KEYS);

    TabletMetaSharedPtr tablet_meta = std::make_shared<TabletMeta>();
    create_tablet_meta(tablet_meta.get());

    write_new_version(tablet_meta, 4);
    write_new_version(tablet_meta, 3);
    write_new_version(tablet_meta, 2);

    TabletSharedPtr tablet =
            Tablet::create_tablet_from_meta(tablet_meta, starrocks::StorageEngine::instance()->get_stores()[0]);
    tablet->init();
    init_compaction_context(tablet);

    ASSERT_EQ(3, tablet->version_count());

    {
        auto res = compact(tablet);
        ASSERT_FALSE(res.ok());

        ASSERT_EQ(3, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(3, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(0, versions[0].second);
        ASSERT_EQ(1, versions[1].first);
        ASSERT_EQ(1, versions[1].second);
        ASSERT_EQ(2, versions[2].first);
        ASSERT_EQ(2, versions[2].second);
    }

    sleep(1);

    config::base_compaction_interval_seconds_since_last_operation = 1;

    {
        auto res = compact(tablet);
        ASSERT_TRUE(res.ok());

        ASSERT_EQ(1, tablet->version_count());
        std::vector<Version> versions;
        tablet->list_versions(&versions);
        ASSERT_EQ(1, versions.size());
        ASSERT_EQ(0, versions[0].first);
        ASSERT_EQ(2, versions[0].second);
    }

    config::base_compaction_interval_seconds_since_last_operation = 86400;
}

} // namespace starrocks::vectorized
