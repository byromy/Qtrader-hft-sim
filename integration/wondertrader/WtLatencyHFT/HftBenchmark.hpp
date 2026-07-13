#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace hftbench
{
	struct StageSample
	{
		uint64_t distribution;
		uint64_t decision;
		uint64_t order_path;
		uint64_t api_call;
		uint64_t end_to_end;
	};

	class Collector
	{
	public:
		explicit Collector(uint32_t sample_count)
			: _samples(sample_count), _index(0), _active(false), _complete(0)
		{
		}

		void begin(bool active)
		{
			_active = active;
			if (!_active)
				return;

			_t0 = now();
			_t1 = _t2 = _t3 = _t4 = 0;
		}

		void strategy_enter() { if (_active) _t1 = now(); }
		void order_start() { if (_active) _t2 = now(); }
		void api_enter() { if (_active) _t3 = now(); }
		void api_exit() { if (_active) _t4 = now(); }

		void end()
		{
			if (!_active || _index >= _samples.size())
				return;

			const uint64_t t5 = now();
			StageSample& sample = _samples[_index++];
			if (_t1 >= _t0 && _t2 >= _t1 && _t3 >= _t2 && _t4 >= _t3 && t5 >= _t4)
			{
				sample.distribution = _t1 - _t0;
				sample.decision = _t2 - _t1;
				sample.order_path = _t3 - _t2;
				sample.api_call = _t4 - _t3;
				sample.end_to_end = t5 - _t0;
				_complete++;
			}
			else
			{
				sample = {};
			}
		}

		void report(const char* output_path) const
		{
			std::FILE* output = std::fopen(output_path, "w");
			if (output)
				std::fprintf(output, "metric,count,mean_ns,p50_ns,p90_ns,p99_ns,p99.9_ns,p99.99_ns,max_ns\n");

			report_metric("distribution", &StageSample::distribution, output);
			report_metric("strategy_decision", &StageSample::decision, output);
			report_metric("order_path", &StageSample::order_path, output);
			report_metric("api_call", &StageSample::api_call, output);
			report_metric("end_to_end", &StageSample::end_to_end, output);

			if (output)
				std::fclose(output);
		std::printf("complete_samples=%zu requested_samples=%zu\n", _complete, _samples.size());
		}

	private:
		using Field = uint64_t StageSample::*;

		static uint64_t now()
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		static uint64_t percentile(const std::vector<uint64_t>& values, double p)
		{
			const std::size_t index = static_cast<std::size_t>(
				std::ceil(p * static_cast<double>(values.size())) - 1.0);
			return values[std::min(index, values.size() - 1)];
		}

		void report_metric(const char* name, Field field, std::FILE* output) const
		{
			std::vector<uint64_t> values;
			values.reserve(_complete);
			long double total = 0;
			for (std::size_t i = 0; i < _index; i++)
			{
				const uint64_t value = _samples[i].*field;
				if (value == 0 && field != &StageSample::api_call)
					continue;
				values.push_back(value);
				total += value;
			}

			if (values.empty())
				return;

			std::sort(values.begin(), values.end());
			const double mean = static_cast<double>(total / values.size());
			const uint64_t p50 = percentile(values, 0.50);
			const uint64_t p90 = percentile(values, 0.90);
			const uint64_t p99 = percentile(values, 0.99);
			const uint64_t p999 = percentile(values, 0.999);
			const uint64_t p9999 = percentile(values, 0.9999);
			const uint64_t maximum = values.back();

			std::printf("%-18s mean=%9.2f ns p50=%llu p90=%llu p99=%llu p99.9=%llu p99.99=%llu max=%llu\n",
				name, mean,
				static_cast<unsigned long long>(p50),
				static_cast<unsigned long long>(p90),
				static_cast<unsigned long long>(p99),
				static_cast<unsigned long long>(p999),
				static_cast<unsigned long long>(p9999),
				static_cast<unsigned long long>(maximum));

			if (output)
			{
				std::fprintf(output, "%s,%zu,%.2f,%llu,%llu,%llu,%llu,%llu,%llu\n",
					name, values.size(), mean,
					static_cast<unsigned long long>(p50),
					static_cast<unsigned long long>(p90),
					static_cast<unsigned long long>(p99),
					static_cast<unsigned long long>(p999),
					static_cast<unsigned long long>(p9999),
					static_cast<unsigned long long>(maximum));
			}
		}

		std::vector<StageSample> _samples;
		std::size_t _index;
		bool _active;
		std::size_t _complete;
		uint64_t _t0;
		uint64_t _t1;
		uint64_t _t2;
		uint64_t _t3;
		uint64_t _t4;
	};
}
