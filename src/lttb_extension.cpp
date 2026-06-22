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

// Type-dispatched read/write function pointers. Resolved once at bind time so
// the per-row Update/Finalize loops call a single indirect function instead of
// a 20-case switch on type.id() per row.
// NOTE: These are bare function pointers (trivially copyable, no stateful
// captures) so LTTBFunctionData::Copy() remains correct. Do not replace with
// std::function or stateful lambdas.
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

// Resolve the type-specific read function at bind time. The returned function
// pointer captures the type dispatch so the per-row loop avoids a 20-case
// switch. DECIMAL still reads scale from the LogicalType (an inline accessor,
// not a dispatch).
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

// Downsample the given points to `buckets` using the LTTB algorithm.
// NOTE: This function mutates `points` in place (sorts it) and may std::move
// it entirely when n <= buckets. This is safe because LTTBFinalize is the
// terminal consumer of the state vector.
// If `out_indices` is non-null, fills it with the 0-based sorted-position
// indices of the selected points (for lttb_indices output).
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

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GetLTTBFunction("lttb"));
	loader.RegisterFunction(GetLTTBFunction("largestTriangleThreeBuckets"));
	// Sorted-input fast path: caller guarantees input is ordered by x.
	loader.RegisterFunction(GetLTTBSortedFunction("lttb_sorted"));
	// Indices output: returns BIGINT[] of selected sorted-position indices.
	loader.RegisterFunction(GetLTTBIndicesFunction("lttb_indices"));
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
