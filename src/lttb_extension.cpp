#define DUCKDB_EXTENSION_MAIN

#include "lttb_extension.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/aggregate_function.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {

namespace {

constexpr idx_t MAX_LTTB_POINTS = 1ULL << 30;

// Powers of 10 indexed by DECIMAL scale (max DuckDB DECIMAL scale is 38).
// Used to convert between raw int storage and double for DECIMAL I/O.
static constexpr double POW10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
    1e20, 1e21, 1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28, 1e29,
    1e30, 1e31, 1e32, 1e33, 1e34, 1e35, 1e36, 1e37, 1e38};

struct LTTBPoint {
	double x;
	double y;
};

struct LTTBState {
	std::vector<LTTBPoint> *points = nullptr;
	uint64_t buckets = 0;
	bool has_buckets = false;
};

// Type-dispatched read/write function pointers, resolved once at bind time so
// per-row loops avoid a switch on type.id(). Bare pointers (trivially
// copyable) — do not replace with std::function or stateful lambdas.
using ReadFunc = double (*)(const UnifiedVectorFormat &, idx_t, const LogicalType &);
using WriteFunc = void (*)(Vector &, idx_t, double, const LogicalType &);

// Bind data carrying whether the caller guarantees pre-sorted input (lttb_sorted)
// and the bind-time-resolved read/write function pointers for x and y.
struct LTTBFunctionData : public FunctionData {
	LTTBFunctionData(bool sorted_p, ReadFunc x_read_p, ReadFunc y_read_p, WriteFunc x_write_p, WriteFunc y_write_p)
	    : sorted(sorted_p), x_read(x_read_p), y_read(y_read_p), x_write(x_write_p), y_write(y_write_p) {
	}

	bool sorted;
	ReadFunc x_read;
	ReadFunc y_read;
	WriteFunc x_write;
	WriteFunc y_write;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LTTBFunctionData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LTTBFunctionData>();
		return sorted == other.sorted && x_read == other.x_read && y_read == other.y_read &&
		       x_write == other.x_write && y_write == other.y_write;
	}
};

static bool IsValidPoint(double x, double y) {
	return !std::isnan(x) && !std::isnan(y);
}

// Resolve the type-specific read function at bind time. DECIMAL reads scale
// from the LogicalType (inline accessor, not a dispatch).
static ReadFunc MakeReader(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DOUBLE:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return UnifiedVectorFormat::GetData<double>(d)[d.sel->get_index(r)];
		};
	case LogicalTypeId::FLOAT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<float>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::TINYINT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<int8_t>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::SMALLINT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<int16_t>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::INTEGER:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<int32_t>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::BIGINT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<int64_t>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::UTINYINT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<uint8_t>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::USMALLINT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<uint16_t>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::UINTEGER:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<uint32_t>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::UBIGINT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<uint64_t>(d)[d.sel->get_index(r)]);
		};
	case LogicalTypeId::HUGEINT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			const auto idx = d.sel->get_index(r);
			double result = 0;
			const auto value = UnifiedVectorFormat::GetData<hugeint_t>(d)[idx];
			if (!Hugeint::TryCast(value, result)) {
				throw InvalidInputException("lttb: HUGEINT value out of double range: %s", value.ToString());
			}
			return result;
		};
	case LogicalTypeId::UHUGEINT:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			const auto idx = d.sel->get_index(r);
			double result = 0;
			const auto value = UnifiedVectorFormat::GetData<uhugeint_t>(d)[idx];
			if (!Uhugeint::TryCast(value, result)) {
				throw InvalidInputException("lttb: UHUGEINT value out of double range: %s", value.ToString());
			}
			return result;
		};
	case LogicalTypeId::DATE:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(
			    Date::EpochDays(UnifiedVectorFormat::GetData<date_t>(d)[d.sel->get_index(r)]));
		};
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(Timestamp::GetEpochMicroSeconds(
			    UnifiedVectorFormat::GetData<timestamp_t>(d)[d.sel->get_index(r)]));
		};
	case LogicalTypeId::TIMESTAMP_SEC:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_sec_t>(d)[d.sel->get_index(r)].value);
		};
	case LogicalTypeId::TIMESTAMP_MS:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_ms_t>(d)[d.sel->get_index(r)].value);
		};
	case LogicalTypeId::TIMESTAMP_NS:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &) {
			return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_ns_t>(d)[d.sel->get_index(r)].value);
		};
	case LogicalTypeId::DECIMAL:
		return [](const UnifiedVectorFormat &d, idx_t r, const LogicalType &t) {
			const auto idx = d.sel->get_index(r);
			const auto divisor = POW10[DecimalType::GetScale(t)];
			switch (t.InternalType()) {
			case PhysicalType::INT16:
				return static_cast<double>(UnifiedVectorFormat::GetData<int16_t>(d)[idx]) / divisor;
			case PhysicalType::INT32:
				return static_cast<double>(UnifiedVectorFormat::GetData<int32_t>(d)[idx]) / divisor;
			case PhysicalType::INT64:
				return static_cast<double>(UnifiedVectorFormat::GetData<int64_t>(d)[idx]) / divisor;
			default: {
				double result = 0;
				const auto value = UnifiedVectorFormat::GetData<hugeint_t>(d)[idx];
				if (!Hugeint::TryCast(value, result)) {
					throw InvalidInputException("lttb: DECIMAL value out of double range: %s", value.ToString());
				}
				return result / divisor;
			}
			}
		};
	default:
		throw InternalException("Unsupported LTTB input type: %s", type.ToString());
	}
}

// Resolve the type-specific write function at bind time. Mirrors MakeReader.
static WriteFunc MakeWriter(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DOUBLE:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<double>(v)[o] = val;
		};
	case LogicalTypeId::FLOAT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<float>(v)[o] = static_cast<float>(val);
		};
	case LogicalTypeId::TINYINT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<int8_t>(v)[o] = static_cast<int8_t>(val);
		};
	case LogicalTypeId::SMALLINT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<int16_t>(v)[o] = static_cast<int16_t>(val);
		};
	case LogicalTypeId::INTEGER:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<int32_t>(v)[o] = static_cast<int32_t>(val);
		};
	case LogicalTypeId::BIGINT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<int64_t>(v)[o] = static_cast<int64_t>(val);
		};
	case LogicalTypeId::UTINYINT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<uint8_t>(v)[o] = static_cast<uint8_t>(val);
		};
	case LogicalTypeId::USMALLINT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<uint16_t>(v)[o] = static_cast<uint16_t>(val);
		};
	case LogicalTypeId::UINTEGER:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<uint32_t>(v)[o] = static_cast<uint32_t>(val);
		};
	case LogicalTypeId::UBIGINT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<uint64_t>(v)[o] = static_cast<uint64_t>(val);
		};
	case LogicalTypeId::HUGEINT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			hugeint_t result;
			if (!Hugeint::TryConvert(val, result)) {
				throw InvalidInputException("lttb: double value out of HUGEINT range on write-back: %f", val);
			}
			FlatVector::GetData<hugeint_t>(v)[o] = result;
		};
	case LogicalTypeId::UHUGEINT:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			uhugeint_t result;
			if (!Uhugeint::TryConvert(val, result)) {
				throw InvalidInputException("lttb: double value out of UHUGEINT range on write-back: %f", val);
			}
			FlatVector::GetData<uhugeint_t>(v)[o] = result;
		};
	case LogicalTypeId::DATE:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<date_t>(v)[o] = Date::EpochDaysToDate(static_cast<int32_t>(val));
		};
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<timestamp_t>(v)[o] = Timestamp::FromEpochMicroSeconds(static_cast<int64_t>(val));
		};
	case LogicalTypeId::TIMESTAMP_SEC:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<timestamp_sec_t>(v)[o] = timestamp_sec_t(static_cast<int64_t>(val));
		};
	case LogicalTypeId::TIMESTAMP_MS:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<timestamp_ms_t>(v)[o] = timestamp_ms_t(static_cast<int64_t>(val));
		};
	case LogicalTypeId::TIMESTAMP_NS:
		return [](Vector &v, idx_t o, double val, const LogicalType &) {
			FlatVector::GetData<timestamp_ns_t>(v)[o] = timestamp_ns_t(static_cast<int64_t>(val));
		};
	case LogicalTypeId::DECIMAL:
		return [](Vector &v, idx_t o, double val, const LogicalType &t) {
			const auto multiplier = POW10[DecimalType::GetScale(t)];
			const auto scaled = std::llround(val * multiplier);
			switch (t.InternalType()) {
			case PhysicalType::INT16:
				FlatVector::GetData<int16_t>(v)[o] = static_cast<int16_t>(
				    MaxValue<int64_t>(MinValue<int64_t>(scaled, std::numeric_limits<int16_t>::max()),
				                      std::numeric_limits<int16_t>::min()));
				break;
			case PhysicalType::INT32:
				FlatVector::GetData<int32_t>(v)[o] = static_cast<int32_t>(
				    MaxValue<int64_t>(MinValue<int64_t>(scaled, std::numeric_limits<int32_t>::max()),
				                      std::numeric_limits<int32_t>::min()));
				break;
			case PhysicalType::INT64:
				FlatVector::GetData<int64_t>(v)[o] = scaled;
				break;
			default: {
				hugeint_t result;
				if (!Hugeint::TryConvert(val * multiplier, result)) {
					throw InvalidInputException("lttb: DECIMAL value out of range on write-back: %f", val);
				}
				FlatVector::GetData<hugeint_t>(v)[o] = result;
				break;
			}
			}
		};
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

// Downsample `points` to `buckets` via LTTB. Mutates `points` in place (sorts
// it; may std::move it when n <= buckets — safe as Finalize is the terminal
// consumer). If `out_indices` is non-null, fills it with selected sorted-position
// indices (for lttb_indices).
static std::vector<LTTBPoint> Downsample(std::vector<LTTBPoint> &points, uint64_t buckets, bool skip_sort = false,
                                        std::vector<idx_t> *out_indices = nullptr) {
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
		if (out_indices) {
			out_indices->resize(n);
			for (idx_t i = 0; i < n; i++) {
				(*out_indices)[i] = i;
			}
		}
		return std::move(points);
	}
	if (buckets == 1) {
		if (out_indices) {
			out_indices->clear();
			out_indices->push_back(0);
		}
		return {points.front()};
	}
	if (buckets == 2) {
		if (out_indices) {
			out_indices->clear();
			out_indices->push_back(0);
			out_indices->push_back(n - 1);
		}
		return {points.front(), points.back()};
	}

	std::vector<LTTBPoint> sampled;
	sampled.reserve(NumericCast<idx_t>(buckets));
	sampled.push_back(points.front());
	if (out_indices) {
		out_indices->clear();
		out_indices->reserve(NumericCast<idx_t>(buckets));
		out_indices->push_back(0);
	}

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
		if (out_indices) {
			out_indices->push_back(max_area_index);
		}
		selected_index = max_area_index;
	}

	sampled.push_back(points.back());
	if (out_indices) {
		out_indices->push_back(n - 1);
	}
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

static void LTTBUpdate(Vector inputs[], AggregateInputData &input_data, idx_t input_count, Vector &state_vector,
                       idx_t count) {
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

	// Resolve the bind-time read function pointers once (outside the row loop).
	auto &bind_data = input_data.bind_data->Cast<LTTBFunctionData>();
	const auto x_read = bind_data.x_read;
	const auto y_read = bind_data.y_read;
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

		const auto x = x_read(x_data, row, x_type);
		const auto y = y_read(y_data, row, y_type);
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
			throw InvalidInputException("lttb aggregate state exceeded maximum point count of " +
			                            std::to_string(MAX_LTTB_POINTS));
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
			throw InvalidInputException("lttb aggregate state exceeded maximum point count of " +
			                            std::to_string(MAX_LTTB_POINTS));
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

	auto &bind_data = input_data.bind_data->Cast<LTTBFunctionData>();
	const bool skip_sort = bind_data.sorted;
	// Resolve the bind-time write function pointers once (outside the row loop).
	const auto x_write = bind_data.x_write;
	const auto y_write = bind_data.y_write;

	idx_t total_size = ListVector::GetListSize(result);
	for (idx_t row = 0; row < count; row++) {
		auto &state = *states[state_data.sel->get_index(row)];
		auto sampled = state.has_buckets && state.points ? Downsample(*state.points, state.buckets, skip_sort) : std::vector<LTTBPoint>();
		ListVector::Reserve(result, total_size + sampled.size());

		auto &entry = list_entries[row + offset];
		entry.offset = total_size;
		entry.length = sampled.size();
		for (idx_t i = 0; i < sampled.size(); i++) {
			x_write(x_child, total_size + i, sampled[i].x, x_type);
			y_write(y_child, total_size + i, sampled[i].y, y_type);
		}
		total_size += sampled.size();
	}
	ListVector::SetListSize(result, total_size);
}

// Finalize for lttb_indices: returns BIGINT[] of selected sorted-position indices.
static void LTTBIndicesFinalize(Vector &state_vector, AggregateInputData &input_data, Vector &result, idx_t count,
                                idx_t offset) {
	UnifiedVectorFormat state_data;
	state_vector.ToUnifiedFormat(count, state_data);
	auto states = UnifiedVectorFormat::GetData<LTTBState *>(state_data);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);

	const bool skip_sort =
	    input_data.bind_data && input_data.bind_data->Cast<LTTBFunctionData>().sorted;

	idx_t total_size = ListVector::GetListSize(result);
	for (idx_t row = 0; row < count; row++) {
		auto &state = *states[state_data.sel->get_index(row)];
		std::vector<idx_t> indices;
		if (state.has_buckets && state.points) {
			Downsample(*state.points, state.buckets, skip_sort, &indices);
		}
		ListVector::Reserve(result, total_size + indices.size());
		auto index_output = FlatVector::GetData<int64_t>(ListVector::GetEntry(result));

		auto &entry = list_entries[row + offset];
		entry.offset = total_size;
		entry.length = indices.size();
		for (idx_t i = 0; i < indices.size(); i++) {
			index_output[total_size + i] = static_cast<int64_t>(indices[i]);
		}
		total_size += indices.size();
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

// Concrete function for lttb_indices: same state/update/combine/destroy, but
// different finalize (writes BIGINT[] of selected sorted-position indices).
static AggregateFunction GetLTTBIndicesConcreteFunction(const string &name, const LogicalType &x_type,
                                                        const LogicalType &y_type) {
	return AggregateFunction(name, {x_type, y_type, LogicalType::BIGINT}, LogicalType::LIST(LogicalType::BIGINT),
	                         LTTBStateSize, LTTBInitialize, LTTBUpdate, LTTBCombine, LTTBIndicesFinalize,
	                         FunctionNullHandling::SPECIAL_HANDLING, nullptr, nullptr, LTTBDestroy);
}

static unique_ptr<FunctionData> LTTBBindFunctionImpl(ClientContext &, AggregateFunction &function,
                                                    vector<unique_ptr<Expression>> &arguments, bool sorted,
                                                    bool indices) {
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

	if (indices) {
		function = GetLTTBIndicesConcreteFunction(function.name, x_resolved, y_resolved);
	} else {
		function = GetLTTBConcreteFunction(function.name, x_resolved, y_resolved);
	}
	// Resolve the type-specific read/write function pointers at bind time so
	// the per-row Update/Finalize loops avoid a 20-case switch dispatch.
	return make_uniq<LTTBFunctionData>(sorted, MakeReader(x_resolved), MakeReader(y_resolved),
	                                   MakeWriter(x_resolved), MakeWriter(y_resolved));
}

static unique_ptr<FunctionData> LTTBBindFunction(ClientContext &context, AggregateFunction &function,
                                                 vector<unique_ptr<Expression>> &arguments) {
	return LTTBBindFunctionImpl(context, function, arguments, false, false);
}

static unique_ptr<FunctionData> LTTBSortedBindFunction(ClientContext &context, AggregateFunction &function,
                                                       vector<unique_ptr<Expression>> &arguments) {
	return LTTBBindFunctionImpl(context, function, arguments, true, false);
}

static unique_ptr<FunctionData> LTTBIndicesBindFunction(ClientContext &context, AggregateFunction &function,
                                                        vector<unique_ptr<Expression>> &arguments) {
	return LTTBBindFunctionImpl(context, function, arguments, false, true);
}

static AggregateFunction GetLTTBFunction(const string &name) {
	return AggregateFunction(name, {LogicalType::ANY, LogicalType::ANY, LogicalType::BIGINT}, LogicalType::ANY, nullptr,
	                         nullptr, nullptr, nullptr, nullptr, nullptr, LTTBBindFunction, nullptr);
}

static AggregateFunction GetLTTBSortedFunction(const string &name) {
	return AggregateFunction(name, {LogicalType::ANY, LogicalType::ANY, LogicalType::BIGINT}, LogicalType::ANY, nullptr,
	                         nullptr, nullptr, nullptr, nullptr, nullptr, LTTBSortedBindFunction, nullptr);
}

static AggregateFunction GetLTTBIndicesFunction(const string &name) {
	return AggregateFunction(name, {LogicalType::ANY, LogicalType::ANY, LogicalType::BIGINT}, LogicalType::ANY, nullptr,
	                         nullptr, nullptr, nullptr, nullptr, nullptr, LTTBIndicesBindFunction, nullptr);
}

// ---------------------------------------------------------------------------
// minmax_lttb: MinMax preselection + LTTB. Reference: plotly-resampler
// MinMaxLTTB. Bin-first preselect keeps argmin(y)/argmax(y) per equi-width
// x-range bin, then runs LTTB on the reduced candidate set. See
// docs/architecture.md §4.7 for the algorithm and complexity analysis.
// ---------------------------------------------------------------------------

// Bind data for minmax_lttb: carries sorted flag, default minmax_ratio, and
// the bind-time-resolved read/write function pointers.
struct MinMaxLTTBFunctionData : public FunctionData {
	MinMaxLTTBFunctionData(bool sorted_p, int64_t minmax_ratio_p, ReadFunc x_read_p, ReadFunc y_read_p,
	                        WriteFunc x_write_p, WriteFunc y_write_p)
	    : sorted(sorted_p), minmax_ratio(minmax_ratio_p), x_read(x_read_p), y_read(y_read_p), x_write(x_write_p),
	      y_write(y_write_p) {
	}

	bool sorted;
	int64_t minmax_ratio; // default ratio when the 4th arg is NULL; >1 required
	ReadFunc x_read;
	ReadFunc y_read;
	WriteFunc x_write;
	WriteFunc y_write;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MinMaxLTTBFunctionData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<MinMaxLTTBFunctionData>();
		return sorted == other.sorted && minmax_ratio == other.minmax_ratio;
	}
};

static void MinMaxLTTBUpdate(Vector inputs[], AggregateInputData &input_data, idx_t input_count,
                             Vector &state_vector, idx_t count) {
	D_ASSERT(input_count == 4);

	UnifiedVectorFormat state_data;
	state_vector.ToUnifiedFormat(count, state_data);
	auto states = UnifiedVectorFormat::GetData<LTTBState *>(state_data);

	UnifiedVectorFormat x_data, y_data, buckets_data, ratio_data;
	inputs[0].ToUnifiedFormat(count, x_data);
	inputs[1].ToUnifiedFormat(count, y_data);
	inputs[2].ToUnifiedFormat(count, buckets_data);
	inputs[3].ToUnifiedFormat(count, ratio_data);
	auto buckets_values = UnifiedVectorFormat::GetData<int64_t>(buckets_data);
	auto ratio_values = UnifiedVectorFormat::GetData<int64_t>(ratio_data);

	auto &bind_data = input_data.bind_data->Cast<MinMaxLTTBFunctionData>();
	const auto x_read = bind_data.x_read;
	const auto y_read = bind_data.y_read;
	const auto &x_type = inputs[0].GetType();
	const auto &y_type = inputs[1].GetType();
	const int64_t default_ratio = bind_data.minmax_ratio;

	for (idx_t row = 0; row < count; row++) {
		const auto x_index = x_data.sel->get_index(row);
		const auto y_index = y_data.sel->get_index(row);
		const auto buckets_index = buckets_data.sel->get_index(row);
		const auto ratio_index = ratio_data.sel->get_index(row);
		if (!x_data.validity.RowIsValid(x_index) || !y_data.validity.RowIsValid(y_index) ||
		    !buckets_data.validity.RowIsValid(buckets_index)) {
			continue;
		}
		// minmax_ratio arg may be NULL → use the bind-time default.
		int64_t minmax_ratio = default_ratio;
		if (ratio_data.validity.RowIsValid(ratio_index)) {
			minmax_ratio = ratio_values[ratio_index];
		}

		auto &state = *states[state_data.sel->get_index(row)];
		const auto buckets_input = buckets_values[buckets_index];
		if (buckets_input < 0) {
			throw InvalidInputException("minmax_lttb bucket count must be non-negative");
		}
		if (minmax_ratio <= 1) {
			throw InvalidInputException("minmax_lttb minmax_ratio must be greater than 1");
		}
		const auto buckets = static_cast<uint64_t>(buckets_input);
		if (!state.has_buckets) {
			state.buckets = buckets;
			state.has_buckets = true;
			// Repurpose the unused `buckets` field is not enough for two params;
			// encode minmax_ratio in the upper 32 bits of buckets (ratio <= 2^31).
			state.buckets = (buckets & 0xFFFFFFFFULL) | (static_cast<uint64_t>(minmax_ratio) << 32);
		} else {
			const auto stored_buckets = state.buckets & 0xFFFFFFFFULL;
			const auto stored_ratio = static_cast<int64_t>(state.buckets >> 32);
			if (stored_buckets != buckets) {
				throw InvalidInputException("minmax_lttb bucket count must be constant within each aggregate group");
			}
			if (stored_ratio != minmax_ratio) {
				throw InvalidInputException("minmax_lttb minmax_ratio must be constant within each aggregate group");
			}
		}

		const auto x = x_read(x_data, row, x_type);
		const auto y = y_read(y_data, row, y_type);
		if (!IsValidPoint(x, y)) {
			continue;
		}
		if (!state.points) {
			state.points = new std::vector<LTTBPoint>();
			state.points->reserve(256);
		}
		if (state.points->size() >= MAX_LTTB_POINTS) {
			throw InvalidInputException("minmax_lttb aggregate state exceeded maximum point count of " +
			                            std::to_string(MAX_LTTB_POINTS));
		}
		state.points->push_back({x, y});
	}
}

static void MinMaxLTTBFinalize(Vector &state_vector, AggregateInputData &input_data, Vector &result, idx_t count,
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

	auto &bind_data = input_data.bind_data->Cast<MinMaxLTTBFunctionData>();
	const bool skip_sort = bind_data.sorted;
	const auto x_write = bind_data.x_write;
	const auto y_write = bind_data.y_write;

	idx_t total_size = ListVector::GetListSize(result);
	for (idx_t row = 0; row < count; row++) {
		auto &state = *states[state_data.sel->get_index(row)];
		std::vector<LTTBPoint> sampled;
		if (state.has_buckets && state.points) {
			const auto n_out = static_cast<uint64_t>(state.buckets & 0xFFFFFFFFULL);
			const auto minmax_ratio = static_cast<int64_t>(state.buckets >> 32);
			auto &points = *state.points;
			const auto n = points.size();
			// Degenerate cases bypass bin-first:
			//   n_out=0 → empty; n<=n_out → all points (via Downsample);
			//   ratio<=1 or n/ratio<=n_out → preselect won't help, run
			//   standard LTTB directly (Downsample handles sort via skip_sort).
			if (n_out == 0) {
				sampled = {};
			} else if (n <= n_out) {
				sampled = Downsample(points, n_out, skip_sort);
			} else if (minmax_ratio <= 1 || n / static_cast<uint64_t>(minmax_ratio) <= n_out) {
				sampled = Downsample(points, n_out, skip_sort);
			} else {
				// Bin-first MinMax preselect: O(n) binning + O(k log k) candidate
				// sort. See docs/architecture.md §4.7 for rationale & benchmarks.
				double x_min = std::numeric_limits<double>::max();
				double x_max = std::numeric_limits<double>::lowest();
				idx_t first_idx = 0;
				idx_t last_idx = 0;
				for (idx_t i = 0; i < n; i++) {
					if (points[i].x < x_min) {
						x_min = points[i].x;
						first_idx = i;
					}
					if (points[i].x > x_max) {
						x_max = points[i].x;
						last_idx = i;
					}
				}

				// Step 2: bin interior points (excluding first/last) into equi-width
				// x-range bins, keeping argmin(y)/argmax(y) per bin.
				const auto n_minmax_buckets = (n_out * static_cast<uint64_t>(minmax_ratio)) / 2;
				struct PerBin {
					// First two points seen, kept verbatim so count<=2 bins retain
					// both points even when y is constant (argmin/argmax tracking
					// collapses equal-y points to a single index).
					LTTBPoint first_point;
					LTTBPoint second_point;
					idx_t first_idx;
					idx_t second_idx;
					// argmin(y) / argmax(y) over the bin, used when count > 2.
					LTTBPoint min_point;
					LTTBPoint max_point;
					idx_t min_idx;
					idx_t max_idx;
					uint64_t count;
					bool has_data;
				};
				std::vector<PerBin> bins(n_minmax_buckets);
				const double x_range = x_max - x_min;
				const double inv_bin_width =
				    (x_range > 0.0) ? static_cast<double>(n_minmax_buckets) / x_range : 0.0;

				for (idx_t i = 0; i < n; i++) {
					if (i == first_idx || i == last_idx) {
						continue;
					}
					uint64_t bin_idx;
					if (x_range <= 0.0) {
						bin_idx = 0;
					} else {
						bin_idx = static_cast<uint64_t>((points[i].x - x_min) * inv_bin_width);
						if (bin_idx >= n_minmax_buckets) {
							bin_idx = n_minmax_buckets - 1;
						}
					}
auto &bin = bins[bin_idx];
				if (!bin.has_data) {
					bin.first_point = points[i];
					bin.first_idx = i;
					bin.min_point = points[i];
					bin.max_point = points[i];
					bin.min_idx = i;
					bin.max_idx = i;
					bin.count = 1;
					bin.has_data = true;
				} else {
					bin.count++;
					if (bin.count == 2) {
						bin.second_point = points[i];
						bin.second_idx = i;
					}
					if (points[i].y < bin.min_point.y) {
						bin.min_point = points[i];
						bin.min_idx = i;
					}
					if (points[i].y > bin.max_point.y) {
						bin.max_point = points[i];
						bin.max_idx = i;
					}
				}
				}

				// Step 3: candidate set = first + per-bin argmin/argmax + last.
				// Push min/max in x-order so the subsequent sort is stable.
				std::vector<LTTBPoint> candidates;
				candidates.reserve(2 + 2 * n_minmax_buckets);
				candidates.push_back(points[first_idx]);
				for (auto &bin : bins) {
					if (!bin.has_data) {
						continue;
					}
if (bin.count == 1) {
					candidates.push_back(bin.first_point);
				} else if (bin.count == 2) {
					// Keep both points; argmin/argmax collapse equal-y pairs to one
					// index, so the second point must come from explicit tracking.
					if (bin.first_point.x <= bin.second_point.x) {
						candidates.push_back(bin.first_point);
						candidates.push_back(bin.second_point);
					} else {
						candidates.push_back(bin.second_point);
						candidates.push_back(bin.first_point);
					}
} else {
					// count > 2: keep argmin(y) and argmax(y). When every point in
					// the bin shares the same y, min_idx == max_idx (both the first
					// point), so push a single representative to avoid a duplicate
					// candidate (harmless to LTTB but wastes a slot and skews the
					// next-bucket average).
					if (bin.min_idx == bin.max_idx) {
						candidates.push_back(bin.min_point);
					} else if (bin.min_point.x <= bin.max_point.x) {
						candidates.push_back(bin.min_point);
						candidates.push_back(bin.max_point);
					} else {
						candidates.push_back(bin.max_point);
						candidates.push_back(bin.min_point);
					}
				}
			}
				// Guard against all-x-equal: first_idx == last_idx would duplicate
				// the endpoint. Only push last when it is a distinct point.
				if (last_idx != first_idx) {
					candidates.push_back(points[last_idx]);
				}

				// Step 4: sort candidates by x, then run LTTB (skip_sort=true).
				std::stable_sort(candidates.begin(), candidates.end(),
				                 [](const LTTBPoint &lhs, const LTTBPoint &rhs) { return lhs.x < rhs.x; });
				sampled = Downsample(candidates, n_out, true);
			}
		}
		ListVector::Reserve(result, total_size + sampled.size());

		auto &entry = list_entries[row + offset];
		entry.offset = total_size;
		entry.length = sampled.size();
		for (idx_t i = 0; i < sampled.size(); i++) {
			x_write(x_child, total_size + i, sampled[i].x, x_type);
			y_write(y_child, total_size + i, sampled[i].y, y_type);
		}
		total_size += sampled.size();
	}
	ListVector::SetListSize(result, total_size);
}

static AggregateFunction GetMinMaxLTTBConcreteFunction(const string &name, const LogicalType &x_type,
                                                       const LogicalType &y_type) {
	return AggregateFunction(name, {x_type, y_type, LogicalType::BIGINT, LogicalType::BIGINT},
	                         LTTBReturnType(x_type, y_type), LTTBStateSize, LTTBInitialize, MinMaxLTTBUpdate,
	                         LTTBCombine, MinMaxLTTBFinalize, FunctionNullHandling::SPECIAL_HANDLING, nullptr, nullptr,
	                         LTTBDestroy);
}

static unique_ptr<FunctionData> MinMaxLTTBBindFunction(ClientContext &, AggregateFunction &function,
                                                       vector<unique_ptr<Expression>> &arguments) {
	for (auto &argument : arguments) {
		if (argument->return_type.id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
	}

	const auto &x_type = arguments[0]->return_type;
	const auto &y_type = arguments[1]->return_type;
	const auto &x_resolved = x_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : x_type;
	const auto &y_resolved = y_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : y_type;
	if (!IsLTTBSupportedType(x_resolved) || !IsLTTBSupportedType(y_resolved)) {
		throw InvalidInputException(
		    "minmax_lttb requires numeric or temporal x/y types; got x=%s, y=%s", x_type.ToString(), y_type.ToString());
	}

	function = GetMinMaxLTTBConcreteFunction(function.name, x_resolved, y_resolved);
	// Default minmax_ratio = 4 (matches plotly-resampler MinMaxLTTB default).
	return make_uniq<MinMaxLTTBFunctionData>(false, 4, MakeReader(x_resolved), MakeReader(y_resolved),
	                                          MakeWriter(x_resolved), MakeWriter(y_resolved));
}

static AggregateFunction GetMinMaxLTTBFunction(const string &name) {
	return AggregateFunction(name, {LogicalType::ANY, LogicalType::ANY, LogicalType::BIGINT, LogicalType::BIGINT},
	                         LogicalType::ANY, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
	                         MinMaxLTTBBindFunction, nullptr);
}

static unique_ptr<FunctionData> MinMaxLTTBSortedBindFunction(ClientContext &, AggregateFunction &function,
                                                              vector<unique_ptr<Expression>> &arguments) {
	for (auto &argument : arguments) {
		if (argument->return_type.id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
	}

	const auto &x_type = arguments[0]->return_type;
	const auto &y_type = arguments[1]->return_type;
	const auto &x_resolved = x_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : x_type;
	const auto &y_resolved = y_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : y_type;
	if (!IsLTTBSupportedType(x_resolved) || !IsLTTBSupportedType(y_resolved)) {
		throw InvalidInputException(
		    "minmax_lttb_sorted requires numeric or temporal x/y types; got x=%s, y=%s", x_type.ToString(), y_type.ToString());
	}

	function = GetMinMaxLTTBConcreteFunction(function.name, x_resolved, y_resolved);
	// Default minmax_ratio = 4 (matches plotly-resampler MinMaxLTTB default).
	return make_uniq<MinMaxLTTBFunctionData>(true, 4, MakeReader(x_resolved), MakeReader(y_resolved),
	                                          MakeWriter(x_resolved), MakeWriter(y_resolved));
}

static AggregateFunction GetMinMaxLTTBSortedFunction(const string &name) {
	return AggregateFunction(name, {LogicalType::ANY, LogicalType::ANY, LogicalType::BIGINT, LogicalType::BIGINT},
	                         LogicalType::ANY, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
	                         MinMaxLTTBSortedBindFunction, nullptr);
}

// ---------------------------------------------------------------------------
// bucket_stats(x, y, num_buckets) → STRUCT(bucket_start, bucket_end, count,
// min, max, mean, std, first, last)[]. Equal-count-by-index bucketing (same
// formula as LTTB). Stats over y only; population std (÷N). See
// docs/architecture.md §4.8 for details.
// ---------------------------------------------------------------------------

struct BucketStatsFunctionData : public FunctionData {
	BucketStatsFunctionData(ReadFunc x_read_p, ReadFunc y_read_p, WriteFunc x_write_p, WriteFunc y_write_p)
	    : x_read(x_read_p), y_read(y_read_p), x_write(x_write_p), y_write(y_write_p) {
	}

	ReadFunc x_read;
	ReadFunc y_read;
	WriteFunc x_write;
	WriteFunc y_write;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<BucketStatsFunctionData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<BucketStatsFunctionData>();
		return x_read == other.x_read && y_read == other.y_read;
	}
};

static void BucketStatsUpdate(Vector inputs[], AggregateInputData &input_data, idx_t input_count,
                              Vector &state_vector, idx_t count) {
	D_ASSERT(input_count == 3);

	UnifiedVectorFormat state_data;
	state_vector.ToUnifiedFormat(count, state_data);
	auto states = UnifiedVectorFormat::GetData<LTTBState *>(state_data);

	UnifiedVectorFormat x_data, y_data, buckets_data;
	inputs[0].ToUnifiedFormat(count, x_data);
	inputs[1].ToUnifiedFormat(count, y_data);
	inputs[2].ToUnifiedFormat(count, buckets_data);
	auto buckets_values = UnifiedVectorFormat::GetData<int64_t>(buckets_data);

	auto &bind_data = input_data.bind_data->Cast<BucketStatsFunctionData>();
	const auto x_read = bind_data.x_read;
	const auto y_read = bind_data.y_read;
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
			throw InvalidInputException("bucket_stats bucket count must be non-negative");
		}
		const auto buckets = static_cast<uint64_t>(buckets_input);
		if (!state.has_buckets) {
			state.buckets = buckets;
			state.has_buckets = true;
		} else if (state.buckets != buckets) {
			throw InvalidInputException("bucket_stats bucket count must be constant within each aggregate group");
		}

		const auto x = x_read(x_data, row, x_type);
		const auto y = y_read(y_data, row, y_type);
		if (!IsValidPoint(x, y)) {
			continue;
		}
		if (!state.points) {
			state.points = new std::vector<LTTBPoint>();
			state.points->reserve(256);
		}
		if (state.points->size() >= MAX_LTTB_POINTS) {
			throw InvalidInputException("bucket_stats aggregate state exceeded maximum point count of " +
			                            std::to_string(MAX_LTTB_POINTS));
		}
		state.points->push_back({x, y});
	}
}

static LogicalType BucketStatsReturnType(const LogicalType &x_type, const LogicalType &y_type) {
	child_list_t<LogicalType> struct_children;
	struct_children.emplace_back("bucket_start", x_type);
	struct_children.emplace_back("bucket_end", x_type);
	struct_children.emplace_back("count", LogicalType::BIGINT);
	struct_children.emplace_back("min", y_type);
	struct_children.emplace_back("max", y_type);
	struct_children.emplace_back("mean", LogicalType::DOUBLE);
	struct_children.emplace_back("std", LogicalType::DOUBLE);
	struct_children.emplace_back("first", y_type);
	struct_children.emplace_back("last", y_type);
	return LogicalType::LIST(LogicalType::STRUCT(std::move(struct_children)));
}

static void BucketStatsFinalize(Vector &state_vector, AggregateInputData &input_data, Vector &result, idx_t count,
                                idx_t offset) {
	UnifiedVectorFormat state_data;
	state_vector.ToUnifiedFormat(count, state_data);
	auto states = UnifiedVectorFormat::GetData<LTTBState *>(state_data);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child_vector = ListVector::GetEntry(result);
	auto &struct_entries = StructVector::GetEntries(child_vector);
	// Struct children: bucket_start, bucket_end, count, min, max, mean, std, first, last
	auto &bucket_start_child = *struct_entries[0];
	auto &bucket_end_child = *struct_entries[1];
	auto &count_child = *struct_entries[2];
	auto &min_child = *struct_entries[3];
	auto &max_child = *struct_entries[4];
	auto &mean_child = *struct_entries[5];
	auto &std_child = *struct_entries[6];
	auto &first_child = *struct_entries[7];
	auto &last_child = *struct_entries[8];
	const auto &x_type = bucket_start_child.GetType();
	const auto &y_type = min_child.GetType();

	auto &bind_data = input_data.bind_data->Cast<BucketStatsFunctionData>();
	const auto x_write = bind_data.x_write;
	const auto y_write = bind_data.y_write;

	idx_t total_size = ListVector::GetListSize(result);
	for (idx_t row = 0; row < count; row++) {
		auto &state = *states[state_data.sel->get_index(row)];
		if (state.has_buckets && state.points) {
			auto &points = *state.points;
			// Stable sort by x (bucket_stats does not have a sorted variant).
			std::stable_sort(points.begin(), points.end(),
			                 [](const LTTBPoint &lhs, const LTTBPoint &rhs) { return lhs.x < rhs.x; });
			const auto n = points.size();
			const auto num_buckets = state.buckets;

			if (num_buckets == 0) {
				// empty — fall through to empty entry below.
			} else if (n <= num_buckets) {
				// Each point is its own bucket (count=1, std=0).
				ListVector::Reserve(result, total_size + n);
				for (idx_t i = 0; i < n; i++) {
					x_write(bucket_start_child, total_size, points[i].x, x_type);
					x_write(bucket_end_child, total_size, points[i].x, x_type);
					FlatVector::GetData<int64_t>(count_child)[total_size] = 1;
					y_write(min_child, total_size, points[i].y, y_type);
					y_write(max_child, total_size, points[i].y, y_type);
					FlatVector::GetData<double>(mean_child)[total_size] = points[i].y;
					FlatVector::GetData<double>(std_child)[total_size] = 0.0;
					y_write(first_child, total_size, points[i].y, y_type);
					y_write(last_child, total_size, points[i].y, y_type);
					total_size += 1;
				}
				auto &entry = list_entries[row + offset];
				entry.offset = total_size - n;
				entry.length = n;
				ListVector::SetListSize(result, total_size);
				continue;
			} else if (num_buckets == 1) {
				// Single bucket: all points.
				double min_y = points[0].y, max_y = points[0].y;
				double first_y = points[0].y, last_y = points[n - 1].y;
				double sum = 0, sum_sq = 0;
				for (idx_t i = 0; i < n; i++) {
					const double y = points[i].y;
					min_y = MinValue(min_y, y);
					max_y = MaxValue(max_y, y);
					sum += y;
					sum_sq += y * y;
				}
				const double mean = sum / static_cast<double>(n);
				const double variance = sum_sq / static_cast<double>(n) - mean * mean;
				const double std = std::sqrt(std::max(0.0, variance));
				// Write as a single struct entry.
				ListVector::Reserve(result, total_size + 1);
				x_write(bucket_start_child, total_size, points[0].x, x_type);
				x_write(bucket_end_child, total_size, points[n - 1].x, x_type);
				FlatVector::GetData<int64_t>(count_child)[total_size] = static_cast<int64_t>(n);
				y_write(min_child, total_size, min_y, y_type);
				y_write(max_child, total_size, max_y, y_type);
				FlatVector::GetData<double>(mean_child)[total_size] = mean;
				FlatVector::GetData<double>(std_child)[total_size] = std;
				y_write(first_child, total_size, first_y, y_type);
				y_write(last_child, total_size, last_y, y_type);
				auto &entry = list_entries[row + offset];
				entry.offset = total_size;
				entry.length = 1;
				total_size += 1;
				ListVector::SetListSize(result, total_size);
				continue;
			} else {
				// Equal-count-by-index bucketing (LTTB formula).
				// First and last points are singletons; interior divided into
				// num_buckets-2 buckets of width (n-2)/(num_buckets-2).
				const double bucket_width = static_cast<double>(n - 2) / static_cast<double>(num_buckets - 2);
				for (uint64_t bucket = 0; bucket < num_buckets; bucket++) {
					idx_t b_start, b_end;
					if (bucket == 0) {
						b_start = 0;
						b_end = 1;
					} else if (bucket == num_buckets - 1) {
						b_start = n - 1;
						b_end = n;
					} else {
						b_start = static_cast<idx_t>(std::floor(static_cast<double>(bucket - 1) * bucket_width)) + 1;
						b_end = MinValue<idx_t>(
						    static_cast<idx_t>(std::floor(static_cast<double>(bucket) * bucket_width)) + 1, n - 1);
					}
					const auto b_count = b_end - b_start;
					if (b_count == 0) {
						continue;
					}
					double min_y = points[b_start].y, max_y = points[b_start].y;
					double first_y = points[b_start].y, last_y = points[b_end - 1].y;
					double sum = 0, sum_sq = 0;
					for (idx_t i = b_start; i < b_end; i++) {
						const double y = points[i].y;
						min_y = MinValue(min_y, y);
						max_y = MaxValue(max_y, y);
						sum += y;
						sum_sq += y * y;
					}
					const double mean = sum / static_cast<double>(b_count);
					const double variance = sum_sq / static_cast<double>(b_count) - mean * mean;
					const double std_val = std::sqrt(std::max(0.0, variance));

					ListVector::Reserve(result, total_size + 1);
					x_write(bucket_start_child, total_size, points[b_start].x, x_type);
					x_write(bucket_end_child, total_size, points[b_end - 1].x, x_type);
					FlatVector::GetData<int64_t>(count_child)[total_size] = static_cast<int64_t>(b_count);
					y_write(min_child, total_size, min_y, y_type);
					y_write(max_child, total_size, max_y, y_type);
					FlatVector::GetData<double>(mean_child)[total_size] = mean;
					FlatVector::GetData<double>(std_child)[total_size] = std_val;
					y_write(first_child, total_size, first_y, y_type);
					y_write(last_child, total_size, last_y, y_type);
					total_size += 1;
				}
				auto &entry = list_entries[row + offset];
				entry.offset = total_size - num_buckets;
				entry.length = num_buckets;
				ListVector::SetListSize(result, total_size);
				continue;
			}
		}
		// Empty or no state.
		auto &entry = list_entries[row + offset];
		entry.offset = total_size;
		entry.length = 0;
	}
	ListVector::SetListSize(result, total_size);
}

static AggregateFunction GetBucketStatsConcreteFunction(const string &name, const LogicalType &x_type,
                                                        const LogicalType &y_type) {
	return AggregateFunction(name, {x_type, y_type, LogicalType::BIGINT}, BucketStatsReturnType(x_type, y_type),
	                         LTTBStateSize, LTTBInitialize, BucketStatsUpdate, LTTBCombine, BucketStatsFinalize,
	                         FunctionNullHandling::SPECIAL_HANDLING, nullptr, nullptr, LTTBDestroy);
}

static unique_ptr<FunctionData> BucketStatsBindFunction(ClientContext &, AggregateFunction &function,
                                                        vector<unique_ptr<Expression>> &arguments) {
	for (auto &argument : arguments) {
		if (argument->return_type.id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
	}

	const auto &x_type = arguments[0]->return_type;
	const auto &y_type = arguments[1]->return_type;
	const auto &x_resolved = x_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : x_type;
	const auto &y_resolved = y_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : y_type;
	if (!IsLTTBSupportedType(x_resolved) || !IsLTTBSupportedType(y_resolved)) {
		throw InvalidInputException(
		    "bucket_stats requires numeric or temporal x/y types; got x=%s, y=%s", x_type.ToString(), y_type.ToString());
	}

	function = GetBucketStatsConcreteFunction(function.name, x_resolved, y_resolved);
	return make_uniq<BucketStatsFunctionData>(MakeReader(x_resolved), MakeReader(y_resolved),
	                                           MakeWriter(x_resolved), MakeWriter(y_resolved));
}

static AggregateFunction GetBucketStatsFunction(const string &name) {
	return AggregateFunction(name, {LogicalType::ANY, LogicalType::ANY, LogicalType::BIGINT}, LogicalType::ANY, nullptr,
	                         nullptr, nullptr, nullptr, nullptr, nullptr, BucketStatsBindFunction, nullptr);
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GetLTTBFunction("lttb"));
	loader.RegisterFunction(GetLTTBFunction("largestTriangleThreeBuckets"));
	// Sorted-input fast path: caller guarantees input is ordered by x.
	loader.RegisterFunction(GetLTTBSortedFunction("lttb_sorted"));
	// Indices output: returns BIGINT[] of selected sorted-position indices.
	loader.RegisterFunction(GetLTTBIndicesFunction("lttb_indices"));
	// MinMax preselection + LTTB for large inputs (compute win, no memory win).
	loader.RegisterFunction(GetMinMaxLTTBFunction("minmax_lttb"));
	// Sorted-input fast path for minmax_lttb.
	loader.RegisterFunction(GetMinMaxLTTBSortedFunction("minmax_lttb_sorted"));
	// Per-bucket statistical downsampling for AI agent distribution analysis.
	loader.RegisterFunction(GetBucketStatsFunction("bucket_stats"));
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
