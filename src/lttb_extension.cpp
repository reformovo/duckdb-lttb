#define DUCKDB_EXTENSION_MAIN

#include "lttb_extension.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/aggregate_function.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace duckdb {

namespace {

constexpr idx_t MAX_LTTB_POINTS = idx_t(1) << 30;

struct LTTBPoint {
	double x;
	double y;
};

struct LTTBState {
	std::vector<LTTBPoint> *points = nullptr;
	uint64_t buckets = 0;
	bool has_buckets = false;
};

static bool IsValidPoint(double x, double y) {
	return !std::isnan(x) && !std::isnan(y);
}

static std::vector<LTTBPoint> Downsample(std::vector<LTTBPoint> &points, uint64_t buckets) {
	std::stable_sort(points.begin(), points.end(), [](const LTTBPoint &lhs, const LTTBPoint &rhs) { return lhs.x < rhs.x; });

	if (buckets == 0) {
		return {};
	}
	if (points.size() <= buckets) {
		return points;
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

	const auto bucket_width = static_cast<double>(points.size() - 2) / static_cast<double>(buckets - 2);
	idx_t selected_index = 0;

	for (uint64_t bucket = 0; bucket < buckets - 2; bucket++) {
		const auto current_bucket = static_cast<double>(bucket);
		const auto next_bucket = static_cast<double>(bucket + 1);
		const auto after_next_bucket = static_cast<double>(bucket + 2);
		const auto current_start = static_cast<idx_t>(std::floor(current_bucket * bucket_width)) + 1;
		const auto current_end = MinValue<idx_t>(static_cast<idx_t>(std::floor(next_bucket * bucket_width)) + 1,
		                                      points.size() - 1);
		const auto next_start = static_cast<idx_t>(std::floor(next_bucket * bucket_width)) + 1;
		const auto next_end = MinValue<idx_t>(static_cast<idx_t>(std::floor(after_next_bucket * bucket_width)) + 1,
		                                   points.size());

		double avg_x = 0;
		double avg_y = 0;
		const auto next_count = next_end > next_start ? next_end - next_start : idx_t(1);
		if (next_end > next_start) {
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
	auto x_values = UnifiedVectorFormat::GetData<double>(x_data);
	auto y_values = UnifiedVectorFormat::GetData<double>(y_data);
	auto buckets_values = UnifiedVectorFormat::GetData<int64_t>(buckets_data);

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

		const auto x = x_values[x_index];
		const auto y = y_values[y_index];
		if (!IsValidPoint(x, y)) {
			continue;
		}
		if (!state.points) {
			state.points = new std::vector<LTTBPoint>();
		}
		if (state.points->size() >= MAX_LTTB_POINTS) {
			throw InvalidInputException("lttb aggregate state exceeded maximum point count of %llu", MAX_LTTB_POINTS);
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
			target.points = new std::vector<LTTBPoint>();
		}
		if (target.points->size() + source.points->size() > MAX_LTTB_POINTS) {
			throw InvalidInputException("lttb aggregate state exceeded maximum point count of %llu", MAX_LTTB_POINTS);
		}
		target.points->insert(target.points->end(), source.points->begin(), source.points->end());
	}
}

static void LTTBFinalize(Vector &state_vector, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
	UnifiedVectorFormat state_data;
	state_vector.ToUnifiedFormat(count, state_data);
	auto states = UnifiedVectorFormat::GetData<LTTBState *>(state_data);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child_vector = ListVector::GetEntry(result);
	auto &struct_entries = StructVector::GetEntries(child_vector);
	auto x_output = FlatVector::GetData<double>(*struct_entries[0]);
	auto y_output = FlatVector::GetData<double>(*struct_entries[1]);

	idx_t total_size = ListVector::GetListSize(result);
	for (idx_t row = 0; row < count; row++) {
		auto &state = *states[state_data.sel->get_index(row)];
		auto sampled = state.has_buckets && state.points ? Downsample(*state.points, state.buckets) : std::vector<LTTBPoint>();
		ListVector::Reserve(result, total_size + sampled.size());
		x_output = FlatVector::GetData<double>(*struct_entries[0]);
		y_output = FlatVector::GetData<double>(*struct_entries[1]);

		auto &entry = list_entries[row + offset];
		entry.offset = total_size;
		entry.length = sampled.size();
		for (idx_t i = 0; i < sampled.size(); i++) {
			x_output[total_size + i] = sampled[i].x;
			y_output[total_size + i] = sampled[i].y;
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

static AggregateFunction GetLTTBFunction(const string &name) {
	child_list_t<LogicalType> struct_children;
	struct_children.emplace_back("x", LogicalType::DOUBLE);
	struct_children.emplace_back("y", LogicalType::DOUBLE);
	auto return_type = LogicalType::LIST(LogicalType::STRUCT(std::move(struct_children)));
	return AggregateFunction(name, {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::BIGINT}, return_type,
	                         LTTBStateSize, LTTBInitialize, LTTBUpdate, LTTBCombine, LTTBFinalize,
	                         FunctionNullHandling::SPECIAL_HANDLING, nullptr, nullptr, LTTBDestroy);
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GetLTTBFunction("lttb"));
	loader.RegisterFunction(GetLTTBFunction("largestTriangleThreeBuckets"));
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
