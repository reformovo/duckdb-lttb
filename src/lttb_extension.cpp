#define DUCKDB_EXTENSION_MAIN

#include "lttb_extension.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/aggregate_function.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace duckdb {

namespace {

constexpr idx_t MAX_LTTB_POINTS = 1ULL << 30;

struct LTTBPoint {
	double x;
	double y;
};

struct LTTBState {
	std::vector<LTTBPoint> *points = nullptr;
	uint64_t buckets = 0;
	bool has_buckets = false;
};

// Bind data carrying whether the caller guarantees pre-sorted input (lttb_sorted).
struct LTTBFunctionData : public FunctionData {
	explicit LTTBFunctionData(bool sorted_p) : sorted(sorted_p) {
	}

	bool sorted;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LTTBFunctionData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LTTBFunctionData>();
		return sorted == other.sorted;
	}
};

static bool IsValidPoint(double x, double y) {
	return !std::isnan(x) && !std::isnan(y);
}

// Read a value of the given logical type from a unified-format vector at row_idx
// and convert it to double for the LTTB algorithm. The conversion is monotonic
// for every supported type (numeric, date, timestamp), so sorting on the
// resulting doubles preserves the correct ordering of the original typed values.
static double ReadAsDouble(const UnifiedVectorFormat &data, idx_t row_idx, const LogicalType &type) {
	const auto idx = data.sel->get_index(row_idx);
	// Caller is responsible for the validity check; this helper assumes a valid row.
	switch (type.id()) {
	case LogicalTypeId::DOUBLE:
		return UnifiedVectorFormat::GetData<double>(data)[idx];
	case LogicalTypeId::FLOAT:
		return static_cast<double>(UnifiedVectorFormat::GetData<float>(data)[idx]);
	case LogicalTypeId::TINYINT:
		return static_cast<double>(UnifiedVectorFormat::GetData<int8_t>(data)[idx]);
	case LogicalTypeId::SMALLINT:
		return static_cast<double>(UnifiedVectorFormat::GetData<int16_t>(data)[idx]);
	case LogicalTypeId::INTEGER:
		return static_cast<double>(UnifiedVectorFormat::GetData<int32_t>(data)[idx]);
	case LogicalTypeId::BIGINT:
		return static_cast<double>(UnifiedVectorFormat::GetData<int64_t>(data)[idx]);
	case LogicalTypeId::UTINYINT:
		return static_cast<double>(UnifiedVectorFormat::GetData<uint8_t>(data)[idx]);
	case LogicalTypeId::USMALLINT:
		return static_cast<double>(UnifiedVectorFormat::GetData<uint16_t>(data)[idx]);
	case LogicalTypeId::UINTEGER:
		return static_cast<double>(UnifiedVectorFormat::GetData<uint32_t>(data)[idx]);
	case LogicalTypeId::UBIGINT:
		return static_cast<double>(UnifiedVectorFormat::GetData<uint64_t>(data)[idx]);
	case LogicalTypeId::HUGEINT: {
		double result = 0;
		Hugeint::TryCast(UnifiedVectorFormat::GetData<hugeint_t>(data)[idx], result);
		return result;
	}
	case LogicalTypeId::UHUGEINT: {
		double result = 0;
		Uhugeint::TryCast(UnifiedVectorFormat::GetData<uhugeint_t>(data)[idx], result);
		return result;
	}
	case LogicalTypeId::DATE:
		// date_t is days since 1970-01-01; lossless double round-trip.
		return static_cast<double>(Date::EpochDays(UnifiedVectorFormat::GetData<date_t>(data)[idx]));
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		// timestamp_t / timestamp_tz_t hold microseconds since epoch; losslessly
		// representable in a double (53-bit mantissa) for all practical dates
		// (until ~2250 CE).
		return static_cast<double>(Timestamp::GetEpochMicroSeconds(
		    UnifiedVectorFormat::GetData<timestamp_t>(data)[idx]));
	case LogicalTypeId::TIMESTAMP_SEC:
		// timestamp_sec_t holds seconds since epoch directly.
		return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_sec_t>(data)[idx].value);
	case LogicalTypeId::TIMESTAMP_MS:
		// timestamp_ms_t holds milliseconds since epoch directly.
		return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_ms_t>(data)[idx].value);
	case LogicalTypeId::TIMESTAMP_NS:
		// timestamp_ns_t holds nanoseconds since epoch directly.
		return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_ns_t>(data)[idx].value);
	case LogicalTypeId::DECIMAL: {
		// Round-trip through double is lossy for DECIMALs with >15 significant
		// digits. Acceptable for LTTB's visualization use case.
		const auto scale = DecimalType::GetScale(type);
		double divisor = 1.0;
		for (uint8_t i = 0; i < scale; i++) {
			divisor *= 10.0;
		}
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return static_cast<double>(UnifiedVectorFormat::GetData<int16_t>(data)[idx]) / divisor;
		case PhysicalType::INT32:
			return static_cast<double>(UnifiedVectorFormat::GetData<int32_t>(data)[idx]) / divisor;
		case PhysicalType::INT64:
			return static_cast<double>(UnifiedVectorFormat::GetData<int64_t>(data)[idx]) / divisor;
		default: {
			double result = 0;
			Hugeint::TryCast(UnifiedVectorFormat::GetData<hugeint_t>(data)[idx], result);
			return result / divisor;
		}
		}
	}
	default:
		throw InternalException("Unsupported LTTB input type: %s", type.ToString());
	}
}

// Write a double value back into a flat vector at the given offset, converting
// it to the vector's logical type. Inverse of ReadAsDouble.
static void WriteFromDouble(Vector &vec, idx_t offset, double value, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DOUBLE:
		FlatVector::GetData<double>(vec)[offset] = value;
		break;
	case LogicalTypeId::FLOAT:
		FlatVector::GetData<float>(vec)[offset] = static_cast<float>(value);
		break;
	case LogicalTypeId::TINYINT:
		FlatVector::GetData<int8_t>(vec)[offset] = static_cast<int8_t>(value);
		break;
	case LogicalTypeId::SMALLINT:
		FlatVector::GetData<int16_t>(vec)[offset] = static_cast<int16_t>(value);
		break;
	case LogicalTypeId::INTEGER:
		FlatVector::GetData<int32_t>(vec)[offset] = static_cast<int32_t>(value);
		break;
	case LogicalTypeId::BIGINT:
		FlatVector::GetData<int64_t>(vec)[offset] = static_cast<int64_t>(value);
		break;
	case LogicalTypeId::UTINYINT:
		FlatVector::GetData<uint8_t>(vec)[offset] = static_cast<uint8_t>(value);
		break;
	case LogicalTypeId::USMALLINT:
		FlatVector::GetData<uint16_t>(vec)[offset] = static_cast<uint16_t>(value);
		break;
	case LogicalTypeId::UINTEGER:
		FlatVector::GetData<uint32_t>(vec)[offset] = static_cast<uint32_t>(value);
		break;
	case LogicalTypeId::UBIGINT:
		FlatVector::GetData<uint64_t>(vec)[offset] = static_cast<uint64_t>(value);
		break;
	case LogicalTypeId::HUGEINT: {
		hugeint_t result;
		Hugeint::TryConvert(value, result);
		FlatVector::GetData<hugeint_t>(vec)[offset] = result;
		break;
	}
	case LogicalTypeId::UHUGEINT: {
		uhugeint_t result;
		Uhugeint::TryConvert(value, result);
		FlatVector::GetData<uhugeint_t>(vec)[offset] = result;
		break;
	}
	case LogicalTypeId::DATE:
		FlatVector::GetData<date_t>(vec)[offset] = Date::EpochDaysToDate(static_cast<int32_t>(value));
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		FlatVector::GetData<timestamp_t>(vec)[offset] =
		    Timestamp::FromEpochMicroSeconds(static_cast<int64_t>(value));
		break;
	case LogicalTypeId::TIMESTAMP_SEC:
		FlatVector::GetData<timestamp_sec_t>(vec)[offset] =
		    timestamp_sec_t(static_cast<int64_t>(value));
		break;
	case LogicalTypeId::TIMESTAMP_MS:
		FlatVector::GetData<timestamp_ms_t>(vec)[offset] =
		    timestamp_ms_t(static_cast<int64_t>(value));
		break;
	case LogicalTypeId::TIMESTAMP_NS:
		FlatVector::GetData<timestamp_ns_t>(vec)[offset] =
		    timestamp_ns_t(static_cast<int64_t>(value));
		break;
	case LogicalTypeId::DECIMAL: {
		const auto scale = DecimalType::GetScale(type);
		double multiplier = 1.0;
		for (uint8_t i = 0; i < scale; i++) {
			multiplier *= 10.0;
		}
		const auto scaled = std::llround(value * multiplier);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			FlatVector::GetData<int16_t>(vec)[offset] = static_cast<int16_t>(scaled);
			break;
		case PhysicalType::INT32:
			FlatVector::GetData<int32_t>(vec)[offset] = static_cast<int32_t>(scaled);
			break;
		case PhysicalType::INT64:
			FlatVector::GetData<int64_t>(vec)[offset] = static_cast<int64_t>(scaled);
			break;
		default: {
			hugeint_t result;
			Hugeint::TryConvert(value * multiplier, result);
			FlatVector::GetData<hugeint_t>(vec)[offset] = result;
			break;
		}
		}
		break;
	}
	default:
		throw InternalException("Unsupported LTTB output type: %s", type.ToString());
	}
}

static bool IsLTTBSupportedType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::DECIMAL:
		return true;
	default:
		return false;
	}
}

// Downsample the given points to `buckets` using the LTTB algorithm.
// NOTE: This function mutates `points` in place (sorts it) and may std::move
// it entirely when n <= buckets. This is safe because LTTBFinalize is the
// terminal consumer of the state vector.
static std::vector<LTTBPoint> Downsample(std::vector<LTTBPoint> &points, uint64_t buckets, bool skip_sort = false) {
	// No buckets requested — return empty immediately, no sorting needed.
	if (buckets == 0) {
		return {};
	}

	if (!skip_sort) {
		// Stable sort: preserve insertion order for equal x values.
		// Epoch-double conversion is monotonic for all supported types, so sorting
		// on doubles preserves correct temporal ordering.
		std::stable_sort(points.begin(), points.end(),
		                 [](const LTTBPoint &lhs, const LTTBPoint &rhs) { return lhs.x < rhs.x; });
	}

	const auto n = points.size();

	// All points fit in the requested buckets — hand back the full sorted set.
	if (n <= buckets) {
		return std::move(points);
	}
	if (buckets == 1) {
		return {points.front()};
	}
	if (buckets == 2) {
		return {points.front(), points.back()};
	}

	std::vector<LTTBPoint> sampled;
	sampled.reserve(NumericCast<idx_t>(buckets));
	sampled.push_back(points.front());

	const double bucket_width = static_cast<double>(n - 2) / static_cast<double>(buckets - 2);
	idx_t selected_index = 0;

	for (uint64_t bucket = 0; bucket < buckets - 2; bucket++) {
		const auto current_bucket = static_cast<double>(bucket);
		const auto next_bucket = static_cast<double>(bucket + 1);
		const auto after_next_bucket = static_cast<double>(bucket + 2);
		const auto current_start = static_cast<idx_t>(std::floor(current_bucket * bucket_width)) + 1;
		const auto current_end = MinValue<idx_t>(static_cast<idx_t>(std::floor(next_bucket * bucket_width)) + 1,
		                                         n - 1);
		const auto next_start = static_cast<idx_t>(std::floor(next_bucket * bucket_width)) + 1;
		const auto next_end =
		    MinValue<idx_t>(static_cast<idx_t>(std::floor(after_next_bucket * bucket_width)) + 1, n);

		double avg_x = 0;
		double avg_y = 0;
		if (next_end > next_start) {
			const auto next_count = next_end - next_start;
			for (idx_t i = next_start; i < next_end; i++) {
				avg_x += points[i].x;
				avg_y += points[i].y;
			}
			avg_x /= static_cast<double>(next_count);
			avg_y /= static_cast<double>(next_count);
		} else {
			avg_x = points.back().x;
			avg_y = points.back().y;
		}

		const auto &previous = points[selected_index];
		double max_area = -1;
		idx_t max_area_index = current_start;
		for (idx_t i = current_start; i < current_end; i++) {
			const auto &candidate = points[i];
			const auto area = std::abs((previous.x - avg_x) * (candidate.y - previous.y) -
			                           (previous.x - candidate.x) * (avg_y - previous.y));
			if (area > max_area) {
				max_area = area;
				max_area_index = i;
			}
		}

		sampled.push_back(points[max_area_index]);
		selected_index = max_area_index;
	}

	sampled.push_back(points.back());
	return sampled;
}

static idx_t LTTBStateSize(const AggregateFunction &) {
	return sizeof(LTTBState);
}

static void LTTBInitialize(const AggregateFunction &, data_ptr_t state) {
	auto &lttb_state = *reinterpret_cast<LTTBState *>(state);
	lttb_state.points = nullptr;
	lttb_state.buckets = 0;
	lttb_state.has_buckets = false;
}

static void LTTBUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &state_vector, idx_t count) {
	D_ASSERT(input_count == 3);

	UnifiedVectorFormat state_data;
	state_vector.ToUnifiedFormat(count, state_data);
	auto states = UnifiedVectorFormat::GetData<LTTBState *>(state_data);

	UnifiedVectorFormat x_data;
	UnifiedVectorFormat y_data;
	UnifiedVectorFormat buckets_data;
	inputs[0].ToUnifiedFormat(count, x_data);
	inputs[1].ToUnifiedFormat(count, y_data);
	inputs[2].ToUnifiedFormat(count, buckets_data);
	auto buckets_values = UnifiedVectorFormat::GetData<int64_t>(buckets_data);

	const auto &x_type = inputs[0].GetType();
	const auto &y_type = inputs[1].GetType();

	for (idx_t row = 0; row < count; row++) {
		const auto x_index = x_data.sel->get_index(row);
		const auto y_index = y_data.sel->get_index(row);
		const auto buckets_index = buckets_data.sel->get_index(row);
		if (!x_data.validity.RowIsValid(x_index) || !y_data.validity.RowIsValid(y_index) ||
		    !buckets_data.validity.RowIsValid(buckets_index)) {
			continue;
		}

		auto &state = *states[state_data.sel->get_index(row)];
		const auto buckets_input = buckets_values[buckets_index];
		if (buckets_input < 0) {
			throw InvalidInputException("lttb bucket count must be non-negative");
		}
		const auto buckets = static_cast<uint64_t>(buckets_input);
		if (!state.has_buckets) {
			state.buckets = buckets;
			state.has_buckets = true;
		} else if (state.buckets != buckets) {
			throw InvalidInputException("lttb bucket count must be constant within each aggregate group");
		}

		const auto x = ReadAsDouble(x_data, row, x_type);
		const auto y = ReadAsDouble(y_data, row, y_type);
		if (!IsValidPoint(x, y)) {
			continue;
		}
		if (!state.points) {
			state.points = new std::vector<LTTBPoint>();
			// Reserve a modest initial capacity to avoid the early geometric
			// reallocations (0→1→2→4→...→256). Capped at 256 to avoid
			// over-allocation in many-group scenarios.
			state.points->reserve(256);
		}
		if (state.points->size() >= MAX_LTTB_POINTS) {
			throw InvalidInputException("lttb aggregate state exceeded maximum point count of %llu",
			                            static_cast<unsigned long long>(MAX_LTTB_POINTS));
		}
		state.points->push_back({x, y});
	}
}

static void LTTBCombine(Vector &state_vector, Vector &combined, AggregateInputData &, idx_t count) {
	UnifiedVectorFormat source_data;
	UnifiedVectorFormat target_data;
	state_vector.ToUnifiedFormat(count, source_data);
	combined.ToUnifiedFormat(count, target_data);
	auto source_states = UnifiedVectorFormat::GetData<LTTBState *>(source_data);
	auto target_states = UnifiedVectorFormat::GetData<LTTBState *>(target_data);

	for (idx_t row = 0; row < count; row++) {
		auto &source = *source_states[source_data.sel->get_index(row)];
		auto &target = *target_states[target_data.sel->get_index(row)];
		if (!source.has_buckets) {
			continue;
		}
		if (!target.has_buckets) {
			target.buckets = source.buckets;
			target.has_buckets = true;
		} else if (target.buckets != source.buckets) {
			throw InvalidInputException("lttb bucket count must be constant within each aggregate group");
		}
		if (!source.points || source.points->empty()) {
			continue;
		}
		if (!target.points) {
			// Target has no points — take ownership of the source vector entirely
			// (O(1) pointer transfer). Source's points pointer is nulled so
			// LTTBDestroy won't double-free.
			target.points = source.points;
			source.points = nullptr;
			continue;
		}
		if (target.points->size() + source.points->size() > MAX_LTTB_POINTS) {
			throw InvalidInputException("lttb aggregate state exceeded maximum point count of %llu",
			                            static_cast<unsigned long long>(MAX_LTTB_POINTS));
		}
		// Reserve before insert to avoid geometric reallocation per combine.
		target.points->reserve(target.points->size() + source.points->size());
		target.points->insert(target.points->end(), source.points->begin(), source.points->end());
	}
}

static void LTTBFinalize(Vector &state_vector, AggregateInputData &input_data, Vector &result, idx_t count,
                         idx_t offset) {
	UnifiedVectorFormat state_data;
	state_vector.ToUnifiedFormat(count, state_data);
	auto states = UnifiedVectorFormat::GetData<LTTBState *>(state_data);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child_vector = ListVector::GetEntry(result);
	auto &struct_entries = StructVector::GetEntries(child_vector);
	auto &x_child = *struct_entries[0];
	auto &y_child = *struct_entries[1];
	const auto &x_type = x_child.GetType();
	const auto &y_type = y_child.GetType();

	const bool skip_sort =
	    input_data.bind_data && input_data.bind_data->Cast<LTTBFunctionData>().sorted;

	idx_t total_size = ListVector::GetListSize(result);
	for (idx_t row = 0; row < count; row++) {
		auto &state = *states[state_data.sel->get_index(row)];
		auto sampled = state.has_buckets && state.points ? Downsample(*state.points, state.buckets, skip_sort) : std::vector<LTTBPoint>();
		ListVector::Reserve(result, total_size + sampled.size());

		auto &entry = list_entries[row + offset];
		entry.offset = total_size;
		entry.length = sampled.size();
		for (idx_t i = 0; i < sampled.size(); i++) {
			WriteFromDouble(x_child, total_size + i, sampled[i].x, x_type);
			WriteFromDouble(y_child, total_size + i, sampled[i].y, y_type);
		}
		total_size += sampled.size();
	}
	ListVector::SetListSize(result, total_size);
}

static void LTTBDestroy(Vector &state_vector, AggregateInputData &, idx_t count) {
	UnifiedVectorFormat state_data;
	state_vector.ToUnifiedFormat(count, state_data);
	auto states = UnifiedVectorFormat::GetData<LTTBState *>(state_data);
	for (idx_t row = 0; row < count; row++) {
		auto &state = *states[state_data.sel->get_index(row)];
		delete state.points;
		state.points = nullptr;
	}
}

// Preserve both x and y types in the output for full type fidelity, matching
// ClickHouse Array(Tuple(typed_x, typed_y)) semantics.
static LogicalType LTTBReturnType(const LogicalType &x_type, const LogicalType &y_type) {
	child_list_t<LogicalType> struct_children;
	struct_children.emplace_back("x", x_type);
	struct_children.emplace_back("y", y_type);
	return LogicalType::LIST(LogicalType::STRUCT(std::move(struct_children)));
}

static AggregateFunction GetLTTBConcreteFunction(const string &name, const LogicalType &x_type,
                                                 const LogicalType &y_type) {
	return AggregateFunction(name, {x_type, y_type, LogicalType::BIGINT}, LTTBReturnType(x_type, y_type),
	                         LTTBStateSize, LTTBInitialize, LTTBUpdate, LTTBCombine, LTTBFinalize,
	                         FunctionNullHandling::SPECIAL_HANDLING, nullptr, nullptr, LTTBDestroy);
}

static unique_ptr<FunctionData> LTTBBindFunctionImpl(ClientContext &, AggregateFunction &function,
                                                    vector<unique_ptr<Expression>> &arguments, bool sorted) {
	for (auto &argument : arguments) {
		if (argument->return_type.id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
	}

	const auto &x_type = arguments[0]->return_type;
	const auto &y_type = arguments[1]->return_type;
	// SQLNULL (untyped NULL literal) coerces to DOUBLE for backward compatibility.
	const auto &x_resolved = x_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : x_type;
	const auto &y_resolved = y_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : y_type;
	if (!IsLTTBSupportedType(x_resolved) || !IsLTTBSupportedType(y_resolved)) {
		throw InvalidInputException(
		    "lttb requires numeric or temporal x/y types; got x=%s, y=%s", x_type.ToString(), y_type.ToString());
	}

	function = GetLTTBConcreteFunction(function.name, x_resolved, y_resolved);
	return make_uniq<LTTBFunctionData>(sorted);
}

static unique_ptr<FunctionData> LTTBBindFunction(ClientContext &context, AggregateFunction &function,
                                                 vector<unique_ptr<Expression>> &arguments) {
	return LTTBBindFunctionImpl(context, function, arguments, false);
}

static unique_ptr<FunctionData> LTTBSortedBindFunction(ClientContext &context, AggregateFunction &function,
                                                       vector<unique_ptr<Expression>> &arguments) {
	return LTTBBindFunctionImpl(context, function, arguments, true);
}

static AggregateFunction GetLTTBFunction(const string &name) {
	return AggregateFunction(name, {LogicalType::ANY, LogicalType::ANY, LogicalType::BIGINT}, LogicalType::ANY, nullptr,
	                         nullptr, nullptr, nullptr, nullptr, nullptr, LTTBBindFunction, nullptr);
}

static AggregateFunction GetLTTBSortedFunction(const string &name) {
	return AggregateFunction(name, {LogicalType::ANY, LogicalType::ANY, LogicalType::BIGINT}, LogicalType::ANY, nullptr,
	                         nullptr, nullptr, nullptr, nullptr, nullptr, LTTBSortedBindFunction, nullptr);
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GetLTTBFunction("lttb"));
	loader.RegisterFunction(GetLTTBFunction("largestTriangleThreeBuckets"));
	// Sorted-input fast path: caller guarantees input is ordered by x.
	loader.RegisterFunction(GetLTTBSortedFunction("lttb_sorted"));
}

void LttbExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string LttbExtension::Name() {
	return "lttb";
}

std::string LttbExtension::Version() const {
#ifdef EXT_VERSION_LTTB
	return EXT_VERSION_LTTB;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(lttb, loader) {
	duckdb::LoadInternal(loader);
}
}
