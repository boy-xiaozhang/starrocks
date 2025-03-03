// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.
#include "exprs/vectorized/bitmap_functions.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "column/array_column.h"
#include "exprs/base64.h"
#include "types/bitmap_value.h"
#include "udf/udf.h"
#include "util/phmap/phmap.h"

namespace starrocks {
namespace vectorized {
class VecBitmapFunctionsTest : public ::testing::Test {
public:
    void SetUp() override {
        ctx_ptr.reset(FunctionContext::create_test_context());
        ctx = ctx_ptr.get();
    }

private:
    std::unique_ptr<FunctionContext> ctx_ptr;
    FunctionContext* ctx;
};

TEST_F(VecBitmapFunctionsTest, bitmapEmptyTest) {
    {
        Columns c;
        auto column = BitmapFunctions::bitmap_empty(ctx, c);

        ASSERT_TRUE(column->is_constant());

        auto* bitmap = ColumnHelper::get_const_value<TYPE_OBJECT>(column);

        ASSERT_EQ(1, bitmap->getSizeInBytes());
    }
}

TEST_F(VecBitmapFunctionsTest, toBitmapTest) {
    {
        Columns columns;

        auto s = BinaryColumn::create();

        s->append(Slice("12312313"));
        s->append(Slice("1"));
        s->append(Slice("0"));

        columns.push_back(s);

        auto column = BitmapFunctions::to_bitmap(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ(5, p->get_object(0)->serialize_size());
        ASSERT_EQ(5, p->get_object(1)->serialize_size());
        ASSERT_EQ(5, p->get_object(2)->serialize_size());
    }

    {
        Columns columns;

        auto s = BinaryColumn::create();

        s->append(Slice("-1"));
        s->append(Slice("1"));
        s->append(Slice("0"));

        columns.push_back(s);

        auto v = BitmapFunctions::to_bitmap(ctx, columns);

        ASSERT_TRUE(v->is_nullable());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(ColumnHelper::as_column<NullableColumn>(v)->data_column());

        ASSERT_TRUE(v->is_null(0));
        ASSERT_EQ(5, p->get_object(1)->serialize_size());
        ASSERT_EQ(5, p->get_object(2)->serialize_size());
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapHashTest) {
    {
        Columns columns;

        auto s = BinaryColumn::create();

        s->append(Slice("12312313"));
        s->append(Slice("1"));
        s->append(Slice("0"));

        columns.push_back(s);

        auto column = BitmapFunctions::bitmap_hash(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ(5, p->get_object(0)->serialize_size());
        ASSERT_EQ(5, p->get_object(1)->serialize_size());
        ASSERT_EQ(5, p->get_object(2)->serialize_size());
    }

    {
        Columns columns;

        auto s = BinaryColumn::create();
        auto n = NullColumn::create();

        s->append(Slice("-1"));
        s->append(Slice("1"));
        s->append(Slice("0"));

        n->append(0);
        n->append(0);
        n->append(1);

        columns.push_back(NullableColumn::create(s, n));

        auto v = BitmapFunctions::bitmap_hash(ctx, columns);

        ASSERT_FALSE(v->is_nullable());
        ASSERT_TRUE(v->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(v);

        ASSERT_EQ(5, p->get_object(0)->serialize_size());
        ASSERT_EQ(5, p->get_object(1)->serialize_size());
        ASSERT_EQ(1, p->get_object(2)->serialize_size());
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapCountTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(2);
    b2.add(3);
    b2.add(4);
    b2.add(2);

    b3.add(0);
    b3.add(0);
    b3.add(0);
    b3.add(0);

    b4.add(4123102120);
    b4.add(23074);
    b4.add(4123123);
    b4.add(23074);

    {
        Columns columns;

        auto s = BitmapColumn::create();

        s->append(&b1);
        s->append(&b2);
        s->append(&b3);
        s->append(&b4);

        columns.push_back(s);

        auto column = BitmapFunctions::bitmap_count(ctx, columns);

        ASSERT_TRUE(column->is_numeric());

        auto p = ColumnHelper::cast_to<TYPE_BIGINT>(column);

        ASSERT_EQ(4, p->get_data()[0]);
        ASSERT_EQ(3, p->get_data()[1]);
        ASSERT_EQ(1, p->get_data()[2]);
        ASSERT_EQ(3, p->get_data()[3]);
    }

    {
        Columns columns;
        auto s = BitmapColumn::create();

        s->append(&b1);
        s->append(&b2);
        s->append(&b3);
        s->append(&b4);

        auto n = NullColumn::create();

        n->append(0);
        n->append(0);
        n->append(1);
        n->append(1);

        columns.push_back(NullableColumn::create(s, n));

        auto v = BitmapFunctions::bitmap_count(ctx, columns);

        ASSERT_FALSE(v->is_nullable());
        ASSERT_TRUE(v->is_numeric());

        auto p = ColumnHelper::cast_to<TYPE_BIGINT>(v);

        ASSERT_EQ(4, p->get_data()[0]);
        ASSERT_EQ(3, p->get_data()[1]);
        ASSERT_EQ(0, p->get_data()[2]);
        ASSERT_EQ(0, p->get_data()[3]);
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapOrTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(1);
    b2.add(2);
    b2.add(3);
    b2.add(4);

    b3.add(1);
    b3.add(2);
    b3.add(3);
    b3.add(4);

    b4.add(4);
    b4.add(5);
    b4.add(6);
    b4.add(7);

    {
        Columns columns;

        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1);
        s1->append(&b2);
        s2->append(&b3);
        s2->append(&b4);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_or(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ(4, p->get_object(0)->cardinality());
        ASSERT_EQ(7, p->get_object(1)->cardinality());
    }

    {
        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1);
        s1->append(&b2);
        s2->append(&b3);
        s2->append(&b4);

        auto n = NullColumn::create();

        n->append(0);
        n->append(1);

        columns.push_back(NullableColumn::create(s1, n));
        columns.push_back(s2);

        auto v = BitmapFunctions::bitmap_or(ctx, columns);

        ASSERT_TRUE(v->is_nullable());
        ASSERT_FALSE(v->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(ColumnHelper::as_column<NullableColumn>(v)->data_column());

        ASSERT_EQ(4, p->get_object(0)->cardinality());
        ASSERT_TRUE(v->is_null(1));
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapAndTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(1);
    b2.add(2);
    b2.add(3);
    b2.add(4);

    b3.add(1);
    b3.add(2);
    b3.add(3);
    b3.add(4);

    b4.add(4);
    b4.add(5);
    b4.add(6);
    b4.add(7);

    {
        Columns columns;

        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1);
        s1->append(&b2);
        s2->append(&b3);
        s2->append(&b4);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_and(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ(4, p->get_object(0)->cardinality());
        ASSERT_EQ(1, p->get_object(1)->cardinality());
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapToStringTest) {
    BitmapValue b1;
    BitmapValue b2;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(4);
    b2.add(5);
    b2.add(6);
    b2.add(7);

    {
        Columns columns;

        auto s1 = BitmapColumn::create();

        s1->append(&b1);
        s1->append(&b2);

        columns.push_back(s1);

        auto column = BitmapFunctions::bitmap_to_string(ctx, columns);

        ASSERT_TRUE(column->is_binary());

        auto p = ColumnHelper::cast_to<TYPE_VARCHAR>(column);

        ASSERT_EQ("1,2,3,4", p->get_slice(0).to_string());
        ASSERT_EQ("4,5,6,7", p->get_slice(1).to_string());
    }

    BitmapValue b3;
    BitmapValue b4;

    // enable bitmap with SET.
    config::enable_bitmap_union_disk_format_with_set = 1;
    b3.add(1);
    b3.add(2);
    b3.add(3);
    b3.add(4);

    b4.add(4);
    b4.add(5);
    b4.add(6);
    b4.add(7);

    {
        Columns columns;

        auto s1 = BitmapColumn::create();

        s1->append(&b3);
        s1->append(&b4);

        columns.push_back(s1);

        auto column = BitmapFunctions::bitmap_to_string(ctx, columns);

        ASSERT_TRUE(column->is_binary());

        auto p = ColumnHelper::cast_to<TYPE_VARCHAR>(column);

        ASSERT_EQ("1,2,3,4", p->get_slice(0).to_string());
        ASSERT_EQ("4,5,6,7", p->get_slice(1).to_string());
    }
    config::enable_bitmap_union_disk_format_with_set = 0;
}

TEST_F(VecBitmapFunctionsTest, bitmapFromStringTest) {
    {
        Columns columns;
        auto s1 = BinaryColumn::create();

        s1->append(Slice("1,2,3,4"));
        s1->append(Slice("4,5,6,7"));

        columns.push_back(s1);

        auto column = BitmapFunctions::bitmap_from_string(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,4", p->get_object(0)->to_string());
        ASSERT_EQ("4,5,6,7", p->get_object(1)->to_string());
    }

    {
        Columns columns;
        auto s1 = BinaryColumn::create();

        s1->append(Slice("1,2,3,4"));
        s1->append(Slice("asdf,7"));

        columns.push_back(s1);

        auto v = BitmapFunctions::bitmap_from_string(ctx, columns);
        ASSERT_TRUE(v->is_nullable());
        ASSERT_FALSE(v->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(ColumnHelper::as_column<NullableColumn>(v)->data_column());

        ASSERT_EQ("1,2,3,4", p->get_object(0)->to_string());
        ASSERT_TRUE(v->is_null(1));
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapContainsTest) {
    BitmapValue b1;
    BitmapValue b2;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(4);
    b2.add(5);
    b2.add(6);
    b2.add(7);
    {
        Columns columns;
        auto s1 = BitmapColumn::create();

        s1->append(&b1);
        s1->append(&b2);

        auto b1 = Int64Column::create();

        b1->append(4);
        b1->append(1);

        columns.push_back(s1);
        columns.push_back(b1);

        auto column = BitmapFunctions::bitmap_contains(ctx, columns);

        ASSERT_TRUE(column->is_numeric());

        auto p = ColumnHelper::cast_to<TYPE_BOOLEAN>(column);

        ASSERT_EQ(1, p->get_data()[0]);
        ASSERT_EQ(0, p->get_data()[1]);
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapHasAnyTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(4);
    b2.add(5);
    b2.add(6);
    b2.add(7);

    b3.add(1);
    b3.add(2);
    b3.add(3);
    b3.add(4);

    b4.add(14);
    b4.add(15);
    b4.add(16);
    b4.add(17);
    {
        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1);
        s1->append(&b2);

        s2->append(&b3);
        s2->append(&b4);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_has_any(ctx, columns);

        ASSERT_TRUE(column->is_numeric());

        auto p = ColumnHelper::cast_to<TYPE_BOOLEAN>(column);

        ASSERT_EQ(1, p->get_data()[0]);
        ASSERT_EQ(0, p->get_data()[1]);
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapNotTest) {
    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);
        b1_column0.add(4);

        BitmapValue b1_column1;
        b1_column1.add(15);
        b1_column1.add(22);
        b1_column1.add(3);
        b1_column1.add(4);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);
        b1_column0.add(4);

        BitmapValue b1_column1;
        b1_column1.add(12);
        b1_column1.add(40);
        b1_column1.add(634);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,4", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);
        b1_column0.add(4);

        BitmapValue b1_column1;
        b1_column1.add(6);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,4", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);
        b1_column0.add(4);

        BitmapValue b1_column1;

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,4", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);

        BitmapValue b1_column1;
        b1_column1.add(15);
        b1_column1.add(22);
        b1_column1.add(3);
        b1_column1.add(4);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);

        BitmapValue b1_column1;
        b1_column1.add(12);
        b1_column1.add(40);
        b1_column1.add(634);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);

        BitmapValue b1_column1;
        b1_column1.add(6);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);

        BitmapValue b1_column1;

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);

        BitmapValue b1_column1;
        b1_column1.add(15);
        b1_column1.add(22);
        b1_column1.add(3);
        b1_column1.add(4);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);

        BitmapValue b1_column1;
        b1_column1.add(12);
        b1_column1.add(40);
        b1_column1.add(634);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);

        BitmapValue b1_column1;
        b1_column1.add(6);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);

        BitmapValue b1_column1;

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;

        BitmapValue b1_column1;
        b1_column1.add(15);
        b1_column1.add(22);
        b1_column1.add(3);
        b1_column1.add(4);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;

        BitmapValue b1_column1;
        b1_column1.add(12);
        b1_column1.add(40);
        b1_column1.add(634);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;

        BitmapValue b1_column1;
        b1_column1.add(6);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;

        BitmapValue b1_column1;

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_andnot(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("", p->get_object(0)->to_string());
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapXorTest) {
    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);
        b1_column0.add(4);

        BitmapValue b1_column1;
        b1_column1.add(15);
        b1_column1.add(22);
        b1_column1.add(3);
        b1_column1.add(4);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,15,22", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);
        b1_column0.add(4);

        BitmapValue b1_column1;
        b1_column1.add(12);
        b1_column1.add(40);
        b1_column1.add(634);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,4,12,40,634", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);
        b1_column0.add(4);

        BitmapValue b1_column1;
        b1_column1.add(6);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,4,6", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);
        b1_column0.add(4);

        BitmapValue b1_column1;

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,4", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);

        BitmapValue b1_column1;
        b1_column1.add(15);
        b1_column1.add(22);
        b1_column1.add(3);
        b1_column1.add(4);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,4,15,22", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);

        BitmapValue b1_column1;
        b1_column1.add(12);
        b1_column1.add(40);
        b1_column1.add(634);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,12,40,634", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);

        BitmapValue b1_column1;
        b1_column1.add(6);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3,6", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);
        b1_column0.add(2);
        b1_column0.add(3);

        BitmapValue b1_column1;

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,2,3", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);

        BitmapValue b1_column1;
        b1_column1.add(15);
        b1_column1.add(22);
        b1_column1.add(3);
        b1_column1.add(4);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,3,4,15,22", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);

        BitmapValue b1_column1;
        b1_column1.add(12);
        b1_column1.add(40);
        b1_column1.add(634);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,12,40,634", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);

        BitmapValue b1_column1;
        b1_column1.add(6);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,6", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;
        b1_column0.add(1);

        BitmapValue b1_column1;

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;

        BitmapValue b1_column1;
        b1_column1.add(15);
        b1_column1.add(22);
        b1_column1.add(3);
        b1_column1.add(4);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("3,4,15,22", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;

        BitmapValue b1_column1;
        b1_column1.add(12);
        b1_column1.add(40);
        b1_column1.add(634);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("12,40,634", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;

        BitmapValue b1_column1;
        b1_column1.add(6);

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("6", p->get_object(0)->to_string());
    }

    {
        BitmapValue b1_column0;

        BitmapValue b1_column1;

        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = BitmapColumn::create();

        s1->append(&b1_column0);
        s2->append(&b1_column1);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_xor(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("", p->get_object(0)->to_string());
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapRemoveTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(1);
    b2.add(4);
    b2.add(634);

    b3.add(634);

    {
        Columns columns;
        auto s1 = BitmapColumn::create();
        auto s2 = Int64Column::create();

        s1->append(&b1);
        s1->append(&b2);
        s1->append(&b3);
        s1->append(&b4);

        s2->append(2);
        s2->append(4);
        s2->append(634);
        s2->append(632);

        columns.push_back(s1);
        columns.push_back(s2);

        auto column = BitmapFunctions::bitmap_remove(ctx, columns);

        ASSERT_TRUE(column->is_object());

        auto p = ColumnHelper::cast_to<TYPE_OBJECT>(column);

        ASSERT_EQ("1,3,4", p->get_object(0)->to_string());
        ASSERT_EQ("1,634", p->get_object(1)->to_string());
        ASSERT_EQ("", p->get_object(2)->to_string());
        ASSERT_EQ("", p->get_object(3)->to_string());
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapToArrayTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(1);
    b2.add(4);
    b2.add(634);

    b3.add(634);

    {
        Columns columns;
        auto s1 = BitmapColumn::create();

        s1->append(&b1);
        s1->append(&b2);
        s1->append(&b3);
        s1->append(&b4);

        columns.push_back(s1);

        auto column = BitmapFunctions::bitmap_to_array(ctx, columns);
        auto array_column = ColumnHelper::as_column<ArrayColumn>(column);

        auto a1 = array_column->get(0).get_array();
        ASSERT_EQ(a1.size(), 4);
        ASSERT_EQ(a1[0].get_int64(), 1);
        ASSERT_EQ(a1[1].get_int64(), 2);
        ASSERT_EQ(a1[2].get_int64(), 3);
        ASSERT_EQ(a1[3].get_int64(), 4);

        auto a2 = array_column->get(1).get_array();
        ASSERT_EQ(a2.size(), 3);
        ASSERT_EQ(a2[0].get_int64(), 1);
        ASSERT_EQ(a2[1].get_int64(), 4);
        ASSERT_EQ(a2[2].get_int64(), 634);

        auto a3 = array_column->get(2).get_array();
        ASSERT_EQ(a3.size(), 1);
        ASSERT_EQ(a3[0].get_int64(), 634);

        auto a4 = array_column->get(3).get_array();
        ASSERT_EQ(a4.size(), 0);
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapToArrayNullTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    b2.add(1);
    b2.add(4);
    b2.add(634);

    b3.add(634);

    {
        Columns columns;
        auto s1 = BitmapColumn::create();

        s1->append(&b1);
        s1->append(&b2);
        s1->append(&b3);
        s1->append(&b4);

        auto n = NullColumn::create();

        n->append(0);
        n->append(0);
        n->append(1);
        n->append(1);

        columns.push_back(NullableColumn::create(s1, n));

        auto column = BitmapFunctions::bitmap_to_array(ctx, columns);
        auto null_column = ColumnHelper::as_column<NullableColumn>(column);
        auto array_column = ColumnHelper::as_column<ArrayColumn>(null_column->data_column());

        auto a1 = array_column->get(0).get_array();
        ASSERT_EQ(a1.size(), 4);
        ASSERT_EQ(a1[0].get_int64(), 1);
        ASSERT_EQ(a1[1].get_int64(), 2);
        ASSERT_EQ(a1[2].get_int64(), 3);
        ASSERT_EQ(a1[3].get_int64(), 4);

        auto a2 = array_column->get(1).get_array();
        ASSERT_EQ(a2.size(), 3);
        ASSERT_EQ(a2[0].get_int64(), 1);
        ASSERT_EQ(a2[1].get_int64(), 4);
        ASSERT_EQ(a2[2].get_int64(), 634);

        ASSERT_TRUE(null_column->is_null(2));
        ASSERT_TRUE(null_column->is_null(3));
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapToArrayConstTest) {
    BitmapValue b1;

    b1.add(1);
    b1.add(2);
    b1.add(3);
    b1.add(4);

    {
        Columns columns;
        auto s1 = BitmapColumn::create();

        s1->append(&b1);

        columns.push_back(ConstColumn::create(s1, 4));

        auto column = BitmapFunctions::bitmap_to_array(ctx, columns);
        auto array_column = ColumnHelper::as_column<ArrayColumn>(column);

        for (size_t i = 0; i < 4; ++i) {
            auto a1 = array_column->get(i).get_array();
            ASSERT_EQ(a1.size(), 4);
            ASSERT_EQ(a1[0].get_int64(), 1);
            ASSERT_EQ(a1[1].get_int64(), 2);
            ASSERT_EQ(a1[2].get_int64(), 3);
            ASSERT_EQ(a1[3].get_int64(), 4);
        }
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapToArrayOnlyNullTest) {
    {
        Columns columns;
        size_t size = 8;
        auto s1 = ColumnHelper::create_const_null_column(size);
        columns.push_back(s1);

        auto column = BitmapFunctions::bitmap_to_array(ctx, columns);
        // auto null_column = ColumnHelper::as_column<NullableColumn>(column);

        for (size_t i = 0; i < size; ++i) {
            ASSERT_TRUE(column->is_null(i));
        }
    }
}

//BitmapValue test
TEST_F(VecBitmapFunctionsTest, bitmapValueUnionOperator) {
    BitmapValue b1;
    int promote_to_bitmap = 33;
    for (int i = 0; i < promote_to_bitmap; ++i) {
        b1.add(i);
    }

    BitmapValue b2;
    b2.add(99);

    {
        BitmapValue a1;
        a1 |= b1;

        BitmapValue a2;
        a2 |= b2;

        BitmapValue a3;
        a3 |= b1;
        a3 |= b2;

        ASSERT_TRUE(a1.cardinality() == promote_to_bitmap);
        ASSERT_TRUE(a2.cardinality() == 1);
        ASSERT_TRUE(a3.cardinality() == (promote_to_bitmap + 1));
    }
}

//BitmapValue test
TEST_F(VecBitmapFunctionsTest, bitmapValueXorOperator) {
    int promote_to_bitmap = 33;

    BitmapValue b1;
    for (int i = 0; i < promote_to_bitmap; ++i) {
        b1.add(i);
    }

    BitmapValue b2;
    for (int i = 0; i < promote_to_bitmap; ++i) {
        b2.add(i);
    }

    {
        b1 ^= b2;
        ASSERT_TRUE(b2.cardinality() == promote_to_bitmap);
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapMaxTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(0);
    b1.add(0);
    b1.add(0);
    b1.add(0);

    b3.add(1);
    b3.add(2);
    b3.add(3);
    b3.add(4);

    b4.add(4123102120);
    b4.add(23074);
    b4.add(4123123);
    b4.add(23074);

    {
        Columns columns;

        auto s = BitmapColumn::create();

        s->append(&b1);
        s->append(&b2);
        s->append(&b3);
        s->append(&b4);

        columns.push_back(s);

        auto column = BitmapFunctions::bitmap_max(ctx, columns);

        ASSERT_TRUE(column->is_numeric());

        auto p = ColumnHelper::cast_to<TYPE_BIGINT>(column);

        ASSERT_EQ(0, p->get_data()[0]);
        ASSERT_EQ(0, p->get_data()[1]);
        ASSERT_EQ(4, p->get_data()[2]);
        ASSERT_EQ(4123102120, p->get_data()[3]);
    }

    {
        Columns columns;
        auto s = BitmapColumn::create();

        s->append(&b1);
        s->append(&b2);
        s->append(&b3);
        s->append(&b4);

        auto n = NullColumn::create();

        n->append(0);
        n->append(1);
        n->append(1);
        n->append(0);

        columns.push_back(NullableColumn::create(s, n));

        auto v = BitmapFunctions::bitmap_max(ctx, columns);

        ASSERT_TRUE(v->is_nullable());

        auto p = ColumnHelper::cast_to<TYPE_BIGINT>(ColumnHelper::as_column<NullableColumn>(v)->data_column());

        ASSERT_EQ(0, p->get_data()[0]);
        ASSERT_TRUE(v->is_null(1));
        ASSERT_TRUE(v->is_null(2));
        ASSERT_EQ(4123102120, p->get_data()[3]);
    }
}

TEST_F(VecBitmapFunctionsTest, bitmapMinTest) {
    BitmapValue b1;
    BitmapValue b2;
    BitmapValue b3;
    BitmapValue b4;

    b1.add(0);
    b1.add(0);
    b1.add(0);
    b1.add(0);

    b3.add(1);
    b3.add(2);
    b3.add(3);
    b3.add(4);

    b4.add(4123102120);
    b4.add(23074);
    b4.add(4123123);
    b4.add(23074);

    {
        Columns columns;

        auto s = BitmapColumn::create();

        s->append(&b1);
        s->append(&b2);
        s->append(&b3);
        s->append(&b4);

        columns.push_back(s);

        auto column = BitmapFunctions::bitmap_min(ctx, columns);

        ASSERT_TRUE(column->is_numeric());

        auto p = ColumnHelper::cast_to<TYPE_BIGINT>(column);

        ASSERT_EQ(0, p->get_data()[0]);
        ASSERT_EQ(-1, p->get_data()[1]);
        ASSERT_EQ(1, p->get_data()[2]);
        ASSERT_EQ(23074, p->get_data()[3]);
    }

    {
        Columns columns;
        auto s = BitmapColumn::create();

        s->append(&b1);
        s->append(&b2);
        s->append(&b3);
        s->append(&b4);

        auto n = NullColumn::create();

        n->append(0);
        n->append(1);
        n->append(1);
        n->append(0);

        columns.push_back(NullableColumn::create(s, n));

        auto v = BitmapFunctions::bitmap_min(ctx, columns);

        ASSERT_TRUE(v->is_nullable());

        auto p = ColumnHelper::cast_to<TYPE_BIGINT>(ColumnHelper::as_column<NullableColumn>(v)->data_column());

        ASSERT_EQ(0, p->get_data()[0]);
        ASSERT_TRUE(v->is_null(1));
        ASSERT_TRUE(v->is_null(2));
        ASSERT_EQ(23074, p->get_data()[3]);
    }
}

TEST_F(VecBitmapFunctionsTest, base64ToBitmapTest) {
    // init bitmap
    BitmapValue bitmap_src({1, 100, 256});

    // init and malloc space
    int size = 1024;
    int len = (size_t)(4.0 * ceil((double)size / 3.0)) + 1;
    char p[len];
    uint8_t* src;
    src = (uint8_t*)malloc(sizeof(uint8_t) * size);

    // serialize and encode bitmap, return char*
    bitmap_src.serialize(src);
    base64_encode2((unsigned char*)src, size, (unsigned char*)p);

    std::unique_ptr<char[]> p1;
    p1.reset(new char[len + 3]);

    // decode and deserialize
    base64_decode2(p, len, p1.get());
    BitmapValue bitmap_decode;
    bitmap_decode.deserialize(p1.get());

    // judge encode and decode bitmap data
    ASSERT_EQ(bitmap_src.to_string(), bitmap_decode.to_string());
    free(src);
}

} // namespace vectorized
} // namespace starrocks
