#include "realm/parser/driver.hpp"
#include "realm/parser/generated/query_bison.hpp"
#include "realm/parser/keypath_mapping.hpp"
#include "realm/parser/query_parser.hpp"
#include "realm/sort_descriptor.hpp"
#include <realm/decimal128.hpp>
#include <realm/uuid.hpp>
#include "realm/util/base64.hpp"

using namespace realm;
using namespace std::string_literals;

// Whether to generate parser debug traces.
static bool trace_parsing = false;
// Whether to generate scanner debug traces.
static bool trace_scanning = false;

namespace {

StringData get_printable_table_name(StringData name)
{
    // the "class_" prefix is an implementation detail of the object store that shouldn't be exposed to users
    static const std::string prefix = "class_";
    if (name.size() > prefix.size() && strncmp(name.data(), prefix.data(), prefix.size()) == 0) {
        name = StringData(name.data() + prefix.size(), name.size() - prefix.size());
    }
    return name;
}

const char* post_op_type_to_str(query_parser::PostOpNode::Type type)
{
    switch (type) {
        case realm::query_parser::PostOpNode::COUNT:
            return ".@count";
        case realm::query_parser::PostOpNode::SIZE:
            return ".@size";
    }
    return "";
}

const char* agg_op_type_to_str(query_parser::AggrNode::Type type)
{
    switch (type) {
        case realm::query_parser::AggrNode::MAX:
            return ".@max";
        case realm::query_parser::AggrNode::MIN:
            return ".@min";
        case realm::query_parser::AggrNode::SUM:
            return ".@sum";
        case realm::query_parser::AggrNode::AVG:
            return ".@avg";
    }
    return "";
}

class MixedArguments : public query_parser::Arguments {
public:
    MixedArguments(const std::vector<Mixed>& args)
        : m_args(args)
    {
    }
    bool bool_for_argument(size_t n) final
    {
        return m_args.at(n).get<bool>();
    }
    long long long_for_argument(size_t n) final
    {
        return m_args.at(n).get<int64_t>();
    }
    float float_for_argument(size_t n) final
    {
        return m_args.at(n).get<float>();
    }
    double double_for_argument(size_t n) final
    {
        return m_args.at(n).get<double>();
    }
    StringData string_for_argument(size_t n) final
    {
        return m_args.at(n).get<StringData>();
    }
    BinaryData binary_for_argument(size_t n) final
    {
        return m_args.at(n).get<BinaryData>();
    }
    Timestamp timestamp_for_argument(size_t n) final
    {
        return m_args.at(n).get<Timestamp>();
    }
    ObjectId objectid_for_argument(size_t n) final
    {
        return m_args.at(n).get<ObjectId>();
    }
    UUID uuid_for_argument(size_t n) final
    {
        return m_args.at(n).get<UUID>();
    }
    Decimal128 decimal128_for_argument(size_t n) final
    {
        return m_args.at(n).get<Decimal128>();
    }
    ObjKey object_index_for_argument(size_t n) final
    {
        return m_args.at(n).get<ObjKey>();
    }
    bool is_argument_null(size_t n) final
    {
        return m_args.at(n).is_null();
    }
    DataType type_for_argument(size_t n)
    {
        return m_args.at(n).get_type();
    }

private:
    const std::vector<Mixed>& m_args;
};

} // namespace

namespace realm {

namespace query_parser {

NoArguments ParserDriver::s_default_args;

Arguments::~Arguments() {}

Timestamp get_timestamp_if_valid(int64_t seconds, int32_t nanoseconds)
{
    const bool both_non_negative = seconds >= 0 && nanoseconds >= 0;
    const bool both_non_positive = seconds <= 0 && nanoseconds <= 0;
    if (both_non_negative || both_non_positive) {
        return Timestamp(seconds, nanoseconds);
    }
    throw std::runtime_error("Invalid timestamp format");
}

ParserNode::~ParserNode() {}

AtomPredNode::~AtomPredNode() {}

Query NotNode::visit(ParserDriver* drv)
{
    Query query = atom_pred->visit(drv);
    Query q = drv->m_base_table->where();
    q.Not();
    q.and_query(query);
    return {q};
}

Query ParensNode::visit(ParserDriver* drv)
{
    return pred->visit(drv);
}

Query OrNode::visit(ParserDriver* drv)
{
    if (and_preds.size() == 1) {
        return and_preds[0]->visit(drv);
    }
    auto it = and_preds.begin();
    auto q = (*it)->visit(drv);
    q.Or();

    ++it;
    while (it != and_preds.end()) {
        q.and_query((*it)->visit(drv));
        ++it;
    }
    return q;
}

Query AndNode::visit(ParserDriver* drv)
{
    if (atom_preds.size() == 1) {
        return atom_preds[0]->visit(drv);
    }
    Query q(drv->m_base_table);
    for (auto it : atom_preds) {
        q.and_query(it->visit(drv));
    }
    return q;
}

Query EqualitylNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    auto left_type = left->get_type();
    auto right_type = right->get_type();

    if (left_type >= 0 && right_type >= 0 && !Mixed::data_types_are_comparable(left_type, right_type)) {
        throw std::runtime_error(util::format("Unsupported comparison between type '%1' and type '%2'",
                                              get_data_type_name(left_type), get_data_type_name(right_type)));
    }

    const ObjPropertyBase* prop = dynamic_cast<const ObjPropertyBase*>(left.get());
    if (prop && !prop->links_exist() && right->has_constant_evaluation() && left_type == right_type) {
        auto col_key = prop->column_key();
        Mixed val = right->get_mixed();
        if (val.is_null()) {
            switch (op) {
                case CompareNode::EQUAL:
                    return drv->m_base_table->where().equal(col_key, realm::null());
                case CompareNode::NOT_EQUAL:
                    return drv->m_base_table->where().not_equal(col_key, realm::null());
            }
        }
        switch (left->get_type()) {
            case type_Int:
                return drv->simple_query(op, col_key, val.get_int());
            case type_Bool:
                return drv->simple_query(op, col_key, val.get_bool());
            case type_String:
                return drv->simple_query(op, col_key, val.get_string(), case_sensitive);
                break;
            case type_Binary:
                return drv->simple_query(op, col_key, val.get_binary(), case_sensitive);
                break;
            case type_Timestamp:
                return drv->simple_query(op, col_key, val.get<Timestamp>());
            case type_Float:
                return drv->simple_query(op, col_key, val.get_float());
                break;
            case type_Double:
                return drv->simple_query(op, col_key, val.get_double());
                break;
            case type_Decimal:
                return drv->simple_query(op, col_key, val.get<Decimal128>());
                break;
            case type_ObjectId:
                break;
            case type_UUID:
                return drv->simple_query(op, col_key, val.get<UUID>());
                break;
            default:
                break;
        }
    }
    if (case_sensitive) {
        switch (op) {
            case CompareNode::EQUAL:
                return Query(std::unique_ptr<Expression>(new Compare<Equal>(std::move(right), std::move(left))));
            case CompareNode::NOT_EQUAL:
                return Query(std::unique_ptr<Expression>(new Compare<NotEqual>(std::move(right), std::move(left))));
        }
    }
    else {
        switch (op) {
            case CompareNode::EQUAL:
                return Query(std::unique_ptr<Expression>(new Compare<EqualIns>(std::move(right), std::move(left))));
            case CompareNode::NOT_EQUAL:
                return Query(
                    std::unique_ptr<Expression>(new Compare<NotEqualIns>(std::move(right), std::move(left))));
        }
    }
    return {};
}

static std::map<int, std::string> opstr = {
    {CompareNode::GREATER, ">"},
    {CompareNode::LESS, "<"},
    {CompareNode::GREATER_EQUAL, ">="},
    {CompareNode::LESS_EQUAL, "<="},
    {CompareNode::BEGINSWITH, "beginswith"},
    {CompareNode::ENDSWITH, "endswith"},
    {CompareNode::CONTAINS, "contains"},
    {CompareNode::LIKE, "like"},
};

Query RelationalNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    auto left_type = left->get_type();
    auto right_type = right->get_type();

    if (left_type == type_UUID) {
        throw std::logic_error(util::format(
            "Unsupported operator %1 in query. Only equal (==) and not equal (!=) are supported for this type.",
            opstr[op]));
    }

    if (left_type < 0 || right_type < 0 || !Mixed::data_types_are_comparable(left_type, right_type)) {
        throw std::runtime_error(util::format("Unsupported comparison between type '%1' and type '%2'",
                                              get_data_type_name(left_type), get_data_type_name(right_type)));
    }

    const ObjPropertyBase* prop = dynamic_cast<const ObjPropertyBase*>(left.get());
    if (prop && !prop->links_exist() && right->has_constant_evaluation() && left_type == right_type) {
        auto col_key = prop->column_key();
        switch (left->get_type()) {
            case type_Int:
                return drv->simple_query(op, col_key, right->get_mixed().get_int());
            case type_Bool:
                break;
            case type_String:
                break;
            case type_Binary:
                break;
            case type_Timestamp:
                return drv->simple_query(op, col_key, right->get_mixed().get<Timestamp>());
            case type_Float:
                return drv->simple_query(op, col_key, right->get_mixed().get_float());
                break;
            case type_Double:
                return drv->simple_query(op, col_key, right->get_mixed().get_double());
                break;
            case type_Decimal:
                return drv->simple_query(op, col_key, right->get_mixed().get<Decimal128>());
                break;
            case type_ObjectId:
                break;
            case type_UUID:
                break;
            default:
                break;
        }
    }
    switch (op) {
        case CompareNode::GREATER:
            return Query(std::unique_ptr<Expression>(new Compare<Less>(std::move(right), std::move(left))));
        case CompareNode::LESS:
            return Query(std::unique_ptr<Expression>(new Compare<Greater>(std::move(right), std::move(left))));
        case CompareNode::GREATER_EQUAL:
            return Query(std::unique_ptr<Expression>(new Compare<LessEqual>(std::move(right), std::move(left))));
        case CompareNode::LESS_EQUAL:
            return Query(std::unique_ptr<Expression>(new Compare<GreaterEqual>(std::move(right), std::move(left))));
    }
    return {};
}

Query StringOpsNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    auto right_type = right->get_type();
    const ObjPropertyBase* prop = dynamic_cast<const ObjPropertyBase*>(left.get());

    if (right_type != type_String && right_type != type_Binary) {
        throw std::runtime_error(util::format(
            "Unsupported comparison operator '%1' against type '%2', right side must be a string or binary type",
            opstr[op], get_data_type_name(right_type)));
    }

    if (prop && !prop->links_exist() && right->has_constant_evaluation() && left->get_type() == right_type) {
        auto col_key = prop->column_key();
        if (right_type == type_String) {
            StringData val = right->get_mixed().get_string();

            switch (op) {
                case CompareNode::BEGINSWITH:
                    return drv->m_base_table->where().begins_with(col_key, val, case_sensitive);
                case CompareNode::ENDSWITH:
                    return drv->m_base_table->where().ends_with(col_key, val, case_sensitive);
                case CompareNode::CONTAINS:
                    return drv->m_base_table->where().contains(col_key, val, case_sensitive);
                case CompareNode::LIKE:
                    return drv->m_base_table->where().like(col_key, val, case_sensitive);
            }
        }
        else if (right_type == type_Binary) {
            BinaryData val = right->get_mixed().get_binary();

            switch (op) {
                case CompareNode::BEGINSWITH:
                    return drv->m_base_table->where().begins_with(col_key, val, case_sensitive);
                case CompareNode::ENDSWITH:
                    return drv->m_base_table->where().ends_with(col_key, val, case_sensitive);
                case CompareNode::CONTAINS:
                    return drv->m_base_table->where().contains(col_key, val, case_sensitive);
                case CompareNode::LIKE:
                    return drv->m_base_table->where().like(col_key, val, case_sensitive);
            }
        }
    }

    if (case_sensitive) {
        switch (op) {
            case CompareNode::BEGINSWITH:
                return Query(std::unique_ptr<Expression>(new Compare<BeginsWith>(std::move(right), std::move(left))));
            case CompareNode::ENDSWITH:
                return Query(std::unique_ptr<Expression>(new Compare<EndsWith>(std::move(right), std::move(left))));
            case CompareNode::CONTAINS:
                return Query(std::unique_ptr<Expression>(new Compare<Contains>(std::move(right), std::move(left))));
            case CompareNode::LIKE:
                return Query(std::unique_ptr<Expression>(new Compare<Like>(std::move(right), std::move(left))));
        }
    }
    else {
        switch (op) {
            case CompareNode::BEGINSWITH:
                return Query(
                    std::unique_ptr<Expression>(new Compare<BeginsWithIns>(std::move(right), std::move(left))));
            case CompareNode::ENDSWITH:
                return Query(
                    std::unique_ptr<Expression>(new Compare<EndsWithIns>(std::move(right), std::move(left))));
            case CompareNode::CONTAINS:
                return Query(
                    std::unique_ptr<Expression>(new Compare<ContainsIns>(std::move(right), std::move(left))));
            case CompareNode::LIKE:
                return Query(std::unique_ptr<Expression>(new Compare<LikeIns>(std::move(right), std::move(left))));
        }
    }
    return {};
}

Query TrueOrFalseNode::visit(ParserDriver* drv)
{
    Query q = drv->m_base_table->where();
    if (true_or_false) {
        q.and_query(std::unique_ptr<realm::Expression>(new TrueExpression));
    }
    else {
        q.and_query(std::unique_ptr<realm::Expression>(new FalseExpression));
    }
    return q;
}

std::unique_ptr<Subexpr> PropNode::visit(ParserDriver* drv)
{
    std::unique_ptr<Subexpr> subexpr{path->visit(drv, comp_type).column(identifier)};

    if (post_op) {
        return post_op->visit(drv, subexpr.get());
    }
    return subexpr;
}

std::unique_ptr<Subexpr> PostOpNode::visit(ParserDriver*, Subexpr* subexpr)
{
    if (auto s = dynamic_cast<Columns<Link>*>(subexpr)) {
        return s->count().clone();
    }
    if (auto s = dynamic_cast<ColumnListBase*>(subexpr)) {
        return s->size().clone();
    }
    if (auto s = dynamic_cast<Columns<StringData>*>(subexpr)) {
        return s->size().clone();
    }
    if (auto s = dynamic_cast<Columns<BinaryData>*>(subexpr)) {
        return s->size().clone();
    }
    if (subexpr) {
        throw std::runtime_error(util::format("Operation '%1' is not supported on property of type '%2'",
                                              post_op_type_to_str(this->type),
                                              get_data_type_name(DataType(subexpr->get_type()))));
    }
    REALM_UNREACHABLE();
    return {};
}

std::unique_ptr<Subexpr> LinkAggrNode::visit(ParserDriver* drv)
{
    auto link_chain = path->visit(drv);
    auto subexpr = std::unique_ptr<Subexpr>(link_chain.column(link));
    auto link_prop = dynamic_cast<Columns<Link>*>(subexpr.get());
    if (!link_prop) {
        throw std::runtime_error(util::format("Operation '%1' cannot apply to property '%2' because it is not a list",
                                              agg_op_type_to_str(aggr_op->type), link));
    }
    auto col_key = link_chain.get_current_table()->get_column_key(prop);

    std::unique_ptr<Subexpr> sub_column;
    switch (col_key.get_type()) {
        case col_type_Int:
            sub_column = link_prop->column<Int>(col_key).clone();
            break;
        case col_type_Float:
            sub_column = link_prop->column<float>(col_key).clone();
            break;
        case col_type_Double:
            sub_column = link_prop->column<double>(col_key).clone();
            break;
        case col_type_Decimal:
            sub_column = link_prop->column<Decimal>(col_key).clone();
            break;
        default:
            throw std::runtime_error(util::format("collection aggregate not supported for type '%1'",
                                                  get_data_type_name(DataType(col_key.get_type()))));
    }
    return aggr_op->visit(drv, sub_column.get());
}

std::unique_ptr<Subexpr> ListAggrNode::visit(ParserDriver* drv)
{
    auto link_chain = path->visit(drv);
    std::unique_ptr<Subexpr> subexpr{link_chain.column(identifier)};
    return aggr_op->visit(drv, subexpr.get());
}

std::unique_ptr<Subexpr> AggrNode::visit(ParserDriver*, Subexpr* subexpr)
{
    if (auto list_prop = dynamic_cast<ColumnListBase*>(subexpr)) {
        switch (type) {
            case MAX:
                return list_prop->max_of();
                break;
            case MIN:
                return list_prop->min_of();
                break;
            case SUM:
                return list_prop->sum_of();
                break;
            case AVG:
                return list_prop->avg_of();
                break;
        }
    }


    if (auto prop = dynamic_cast<SubColumnBase*>(subexpr)) {
        switch (type) {
            case MAX:
                return prop->max_of();
                break;
            case MIN:
                return prop->min_of();
                break;
            case SUM:
                return prop->sum_of();
                break;
            case AVG:
                return prop->avg_of();
                break;
        }
    }

    throw std::runtime_error("Cannot aggregate");
    return {};
}

std::unique_ptr<Subexpr> ConstantNode::visit(ParserDriver* drv, DataType hint)
{
    Subexpr* ret = nullptr;
    switch (type) {
        case Type::NUMBER: {
            if (hint == type_Decimal) {
                ret = new Value<Decimal128>(Decimal128(text));
            }
            else {
                ret = new Value<int64_t>(strtol(text.c_str(), nullptr, 0));
            }
            break;
        }
        case Type::FLOAT: {
            switch (hint) {
                case type_Float: {
                    ret = new Value<float>(strtof(text.c_str(), nullptr));
                    break;
                }
                case type_Decimal:
                    ret = new Value<Decimal128>(Decimal128(text));
                    break;
                default:
                    ret = new Value<double>(strtod(text.c_str(), nullptr));
                    break;
            }
            break;
        }
        case Type::INFINITY_VAL: {
            bool negative = text[0] == '-';
            switch (hint) {
                case type_Float: {
                    auto inf = std::numeric_limits<float>::infinity();
                    ret = new Value<float>(negative ? -inf : inf);
                    break;
                }
                case type_Double: {
                    auto inf = std::numeric_limits<double>::infinity();
                    ret = new Value<double>(negative ? -inf : inf);
                    break;
                }
                case type_Decimal:
                    ret = new Value<Decimal128>(Decimal128(text));
                    break;
                default:
                    throw std::runtime_error(util::format("Infinity not supported for %1", get_data_type_name(hint)));
                    break;
            }
            break;
        }
        case Type::NAN_VAL: {
            switch (hint) {
                case type_Float:
                    ret = new Value<float>(type_punning<float>(0x7fc00000));
                    break;
                case type_Double:
                    ret = new Value<double>(type_punning<double>(0x7ff8000000000000));
                    break;
                case type_Decimal:
                    ret = new Value<Decimal128>(Decimal128::nan("0"));
                    break;
                default:
                    REALM_UNREACHABLE();
                    break;
            }
            break;
        }
        case Type::STRING: {
            std::string str = text.substr(1, text.size() - 2);
            ret = new ConstantStringValue(str);
            break;
        }
        case Type::BASE64: {
            const size_t encoded_size = text.size() - 5;
            size_t buffer_size = util::base64_decoded_size(encoded_size);
            drv->m_args.buffer_space.push_back({});
            auto& decode_buffer = drv->m_args.buffer_space.back();
            decode_buffer.resize(buffer_size);
            StringData window(text.c_str() + 4, encoded_size);
            util::Optional<size_t> decoded_size = util::base64_decode(window, decode_buffer.data(), buffer_size);
            if (!decoded_size) {
                throw std::runtime_error("Invalid base64 value");
            }
            REALM_ASSERT_DEBUG_EX(*decoded_size <= encoded_size, *decoded_size, encoded_size);
            decode_buffer.resize(*decoded_size); // truncate

            if (hint == type_String) {
                ret = new ConstantStringValue(StringData(decode_buffer.data(), decode_buffer.size()));
            }
            if (hint == type_Binary) {
                ret = new Value<BinaryData>(BinaryData(decode_buffer.data(), decode_buffer.size()));
            }
            break;
        }
        case Type::TIMESTAMP: {
            auto s = text;
            int64_t seconds;
            int32_t nanoseconds;
            if (s[0] == 'T') {
                size_t colon_pos = s.find(":");
                std::string s1 = s.substr(1, colon_pos - 1);
                std::string s2 = s.substr(colon_pos + 1);
                seconds = strtol(s1.c_str(), nullptr, 0);
                nanoseconds = int32_t(strtol(s2.c_str(), nullptr, 0));
            }
            else {
                // readable format YYYY-MM-DD-HH:MM:SS:NANOS nanos optional
                struct tm tmp = tm();
                char sep = s.find("@") < s.size() ? '@' : 'T';
                std::string fmt = "%d-%d-%d"s + sep + "%d:%d:%d:%d"s;
                int cnt = sscanf(s.c_str(), fmt.c_str(), &tmp.tm_year, &tmp.tm_mon, &tmp.tm_mday, &tmp.tm_hour,
                                 &tmp.tm_min, &tmp.tm_sec, &nanoseconds);
                REALM_ASSERT(cnt >= 6);
                tmp.tm_year -= 1900; // epoch offset (see man mktime)
                tmp.tm_mon -= 1;     // converts from 1-12 to 0-11

                if (tmp.tm_year < 0) {
                    // platform timegm functions do not throw errors, they return -1 which is also a valid time
                    throw std::logic_error("Conversion of dates before 1900 is not supported.");
                }

                seconds = platform_timegm(tmp); // UTC time
                if (cnt == 6) {
                    nanoseconds = 0;
                }
                if (nanoseconds < 0) {
                    throw std::logic_error("The nanoseconds of a Timestamp cannot be negative.");
                }
                if (seconds < 0) { // seconds determines the sign of the nanoseconds part
                    nanoseconds *= -1;
                }
            }
            ret = new Value<Timestamp>(get_timestamp_if_valid(seconds, nanoseconds));
            break;
        }
        case Type::UUID_T:
            ret = new Value<UUID>(UUID(text.substr(5, text.size() - 6)));
            break;
        case Type::OID:
            ret = new Value<ObjectId>(ObjectId(text.substr(4, text.size() - 5).c_str()));
            break;
        case Type::NULL_VAL:
            if (hint == type_String) {
                ret = new ConstantStringValue(StringData()); // Null string
            }
            else if (hint == type_Binary) {
                ret = new Value<Binary>(BinaryData()); // Null string
            }
            else if (hint == type_LinkList) {
                throw std::runtime_error("Cannot compare linklist with NULL");
            }
            else {
                ret = new Value<null>(realm::null());
            }
            break;
        case Type::TRUE:
            ret = new Value<Bool>(true);
            break;
        case Type::FALSE:
            ret = new Value<Bool>(false);
            break;
        case Type::ARG: {
            size_t arg_no = size_t(strtol(text.substr(1).c_str(), nullptr, 10));
            if (drv->m_args.is_argument_null(arg_no)) {
                ret = new Value<null>(realm::null());
            }
            else {
                auto type = drv->m_args.type_for_argument(arg_no);
                switch (type) {
                    case type_Int:
                        ret = new Value<int64_t>(drv->m_args.long_for_argument(arg_no));
                        break;
                    case type_String:
                        ret = new ConstantStringValue(drv->m_args.string_for_argument(arg_no));
                        break;
                    case type_Binary:
                        ret = new Value<BinaryData>(drv->m_args.binary_for_argument(arg_no));
                        break;
                    case type_Bool:
                        ret = new Value<Bool>(drv->m_args.bool_for_argument(arg_no));
                        break;
                    case type_Float:
                        ret = new Value<float>(drv->m_args.float_for_argument(arg_no));
                        break;
                    case type_Double:
                        ret = new Value<double>(drv->m_args.double_for_argument(arg_no));
                        break;
                    case type_Timestamp: {
                        try {
                            ret = new Value<Timestamp>(drv->m_args.timestamp_for_argument(arg_no));
                        }
                        catch (const std::exception&) {
                            ret = new Value<ObjectId>(drv->m_args.objectid_for_argument(arg_no));
                        }
                        break;
                    }
                    case type_ObjectId: {
                        try {
                            ret = new Value<ObjectId>(drv->m_args.objectid_for_argument(arg_no));
                        }
                        catch (const std::exception&) {
                            ret = new Value<Timestamp>(drv->m_args.timestamp_for_argument(arg_no));
                        }
                        break;
                    }
                    case type_Decimal:
                        ret = new Value<Decimal128>(drv->m_args.decimal128_for_argument(arg_no));
                        break;
                    case type_UUID:
                        ret = new Value<UUID>(drv->m_args.uuid_for_argument(arg_no));
                        break;
                    default:
                        break;
                }
            }
            break;
        }
    }
    if (!ret) {
        throw std::runtime_error(
            util::format("Unsupported comparison between property of type '%1' and constant value '%2'",
                         get_data_type_name(hint), text));
    }
    return std::unique_ptr<Subexpr>{ret};
}

LinkChain PathNode::visit(ParserDriver* drv, ExpressionComparisonType comp_type)
{
    LinkChain link_chain(drv->m_base_table, comp_type);
    for (auto path_elem : path_elems) {
        if (path_elem.find("@links.") == 0) {
            link_chain.backlink(path_elem);
        }
        else {
            link_chain.link(path_elem);
        }
    }
    return link_chain;
}

DescriptorNode::~DescriptorNode() {}

DescriptorOrderingNode::~DescriptorOrderingNode() {}

std::unique_ptr<DescriptorOrdering> DescriptorOrderingNode::visit(ParserDriver* drv)
{
    auto target = drv->m_base_table;
    std::unique_ptr<DescriptorOrdering> ordering;
    for (auto cur_ordering : orderings) {
        if (!ordering)
            ordering = std::make_unique<DescriptorOrdering>();
        if (cur_ordering->get_type() == DescriptorNode::LIMIT) {
            ordering->append_limit(LimitDescriptor(cur_ordering->limit));
        }
        else {
            bool is_distinct = cur_ordering->get_type() == DescriptorNode::DISTINCT;
            std::vector<std::vector<ColKey>> property_columns;
            for (auto& col_names : cur_ordering->columns) {
                std::vector<ColKey> columns;
                ConstTableRef cur_table = target;
                for (size_t ndx_in_path = 0; ndx_in_path < col_names.size(); ++ndx_in_path) {
                    ColKey col_key = cur_table->get_column_key(col_names[ndx_in_path]);
                    if (!col_key) {
                        throw std::runtime_error(util::format(
                            "No property '%1' found on object type '%2' specified in '%3' clause",
                            col_names[ndx_in_path], cur_table->get_name(), is_distinct ? "distinct" : "sort"));
                    }
                    columns.push_back(col_key);
                    if (ndx_in_path < col_names.size() - 1) {
                        cur_table = cur_table->get_link_target(col_key);
                    }
                }
                property_columns.push_back(columns);
            }

            if (is_distinct) {
                ordering->append_distinct(DistinctDescriptor(property_columns));
            }
            else {
                ordering->append_sort(SortDescriptor(property_columns, cur_ordering->ascending),
                                      SortDescriptor::MergeMode::prepend);
            }
        }
    }

    return ordering;
}

std::pair<std::unique_ptr<Subexpr>, std::unique_ptr<Subexpr>> ParserDriver::cmp(const std::vector<ValueNode*>& values)
{
    std::unique_ptr<Subexpr> left;
    std::unique_ptr<Subexpr> right;

    auto left_constant = values[0]->constant;
    auto right_constant = values[1]->constant;
    auto left_prop = values[0]->prop;
    auto right_prop = values[1]->prop;

    if (left_constant && right_constant) {
        throw std::runtime_error("Cannot compare two constants");
    }

    if (right_constant) {
        // Take left first - it cannot be a constant
        left = left_prop->visit(this);
        right = right_constant->visit(this, left->get_type());
    }
    else {
        right = right_prop->visit(this);
        if (left_constant) {
            left = left_constant->visit(this, right->get_type());
        }
        else {
            left = left_prop->visit(this);
        }
    }
    if (dynamic_cast<ColumnListBase*>(left.get()) && dynamic_cast<ColumnListBase*>(right.get())) {
        util::serializer::SerialisationState state;
        throw std::runtime_error(
            util::format("Ordered comparison between two primitive lists is not implemented yet ('%1' and '%2')",
                         left->description(state), right->description(state)));
    }
    return {std::move(left), std::move(right)};
}

int ParserDriver::parse(const std::string& str)
{
    // std::cout << str << std::endl;
    parse_string = str;
    scan_begin(trace_scanning);
    yy::parser parse(*this);
    parse.set_debug_level(trace_parsing);
    int res = parse();
    scan_end();
    if (parse_error) {
        std::string msg = "Invalid predicate: '" + str + "': " + error_string;
        throw std::runtime_error(msg);
    }
    return res;
}

void parse(const std::string& str)
{
    ParserDriver driver;
    driver.parse(str);
}

} // namespace query_parser

Query Table::query(const std::string& query_string, const std::vector<Mixed>& arguments) const
{
    MixedArguments args(arguments);
    return query(query_string, args, {});
}

Query Table::query(const std::string& query_string, query_parser::Arguments& args,
                   const query_parser::KeyPathMapping&) const
{
    ParserDriver driver(m_own_ref, args);
    driver.parse(query_string);
    return driver.result->visit(&driver).set_ordering(driver.ordering->visit(&driver));
}

LinkChain& LinkChain::backlink(const std::string& path_elem)
{
    auto table_column_pair = path_elem.substr(7);
    auto dot_pos = table_column_pair.find('.');
    auto table_name = table_column_pair.substr(0, dot_pos);
    auto column_name = table_column_pair.substr(dot_pos + 1);
    auto origin_table = m_base_table->get_parent_group()->get_table(table_name);
    ColKey origin_column;
    if (origin_table) {
        origin_column = origin_table->get_column_key(column_name);
    }
    if (!origin_column) {
        throw std::runtime_error(util::format("No property '%1' found in type '%2' which links to type '%3'",
                                              column_name, get_printable_table_name(table_name),
                                              get_printable_table_name(m_current_table->get_name())));
    }
    return backlink(*origin_table, origin_column);
}


Subexpr* LinkChain::column(std::string col)
{
    if (col.find("@links.") == 0) {
        backlink(col);
        return new Columns<Link>(ColKey(), m_base_table, m_link_cols, m_comparison_type);
    }

    auto col_key = m_current_table->get_column_key(col);
    if (!col_key) {
        throw std::runtime_error(util::format("'%1' has no property: '%2'", m_current_table->get_name(), col));
    }

    if (col_key.is_list()) {
        switch (col_key.get_type()) {
            case col_type_Int:
                return new Columns<Lst<Int>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Bool:
                return new Columns<Lst<Bool>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_String:
                return new Columns<Lst<String>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Binary:
                return new Columns<Lst<Binary>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Float:
                return new Columns<Lst<Float>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Double:
                return new Columns<Lst<Double>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Timestamp:
                return new Columns<Lst<Timestamp>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Decimal:
                return new Columns<Lst<Decimal>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_UUID:
                return new Columns<Lst<UUID>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_ObjectId:
                return new Columns<Lst<ObjectId>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Mixed:
                return new Columns<Lst<Mixed>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_LinkList:
                add(col_key);
                return new Columns<Link>(col_key, m_base_table, m_link_cols, m_comparison_type);
            default:
                break;
        }
    }
    else {
        switch (col_key.get_type()) {
            case col_type_Int:
                return new Columns<Int>(col_key, m_base_table, m_link_cols);
            case col_type_Bool:
                return new Columns<Bool>(col_key, m_base_table, m_link_cols);
            case col_type_String:
                return new Columns<String>(col_key, m_base_table, m_link_cols);
            case col_type_Binary:
                return new Columns<Binary>(col_key, m_base_table, m_link_cols);
            case col_type_Float:
                return new Columns<Float>(col_key, m_base_table, m_link_cols);
            case col_type_Double:
                return new Columns<Double>(col_key, m_base_table, m_link_cols);
            case col_type_Timestamp:
                return new Columns<Timestamp>(col_key, m_base_table, m_link_cols);
            case col_type_Decimal:
                return new Columns<Decimal128>(col_key, m_base_table, m_link_cols);
            case col_type_UUID:
                return new Columns<UUID>(col_key, m_base_table, m_link_cols);
            case col_type_ObjectId:
                return new Columns<ObjectId>(col_key, m_base_table, m_link_cols);
            case col_type_Mixed:
                return new Columns<Mixed>(col_key, m_base_table, m_link_cols);
            case col_type_Link:
                add(col_key);
                return new Columns<Link>(col_key, m_base_table, m_link_cols);
            default:
                break;
        }
    }
    REALM_UNREACHABLE();
    return nullptr;
}
} // namespace realm
