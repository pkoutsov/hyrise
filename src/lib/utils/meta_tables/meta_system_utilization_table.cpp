#include <chrono>
#include <fstream>
#include <iostream>

#include "sys/types.h"

#if defined __linux__

#include "sys/sysinfo.h"
#include "sys/times.h"

#elif defined __APPLE__

#include "mach/mach.h"
#include "mach/mach_error.h"
#include "mach/mach_host.h"
#include "mach/mach_init.h"
#include "mach/mach_time.h"
#include "mach/vm_map.h"
#include "mach/vm_statistics.h"
#include "sys/resource.h"
#include "sys/sysctl.h"

#endif

#include "meta_system_utilization_table.hpp"

#include "hyrise.hpp"

namespace opossum {

MetaSystemUtilizationTable::MetaSystemUtilizationTable()
    : AbstractMetaTable(TableColumnDefinitions{{"cpu_system_usage", DataType::Float, false},
                                               {"cpu_process_usage", DataType::Float, false},
                                               {"load_average_1_min", DataType::Float, false},
                                               {"load_average_5_min", DataType::Float, false},
                                               {"load_average_15_min", DataType::Float, false},
                                               {"system_memory_free_bytes", DataType::Long, false},
                                               {"process_virtual_memory_bytes", DataType::Long, false},
                                               {"process_physical_memory_bytes", DataType::Long, false}}) {}

const std::string& MetaSystemUtilizationTable::name() const {
  static const auto name = std::string{"system_utilization"};
  return name;
}

void MetaSystemUtilizationTable::init() {
  _get_system_cpu_usage();
  _get_process_cpu_usage();
}

std::shared_ptr<Table> MetaSystemUtilizationTable::_on_generate() {
  auto output_table = std::make_shared<Table>(_column_definitions, TableType::Data, std::nullopt, UseMvcc::Yes);

  const auto system_cpu_usage = _get_system_cpu_usage();
  const auto process_cpu_usage = _get_process_cpu_usage();
  const auto load_avg = _get_load_avg();
  const auto system_memory_usage = _get_system_memory_usage();
  const auto process_memory_usage = _get_process_memory_usage();

  output_table->append({system_cpu_usage, process_cpu_usage, load_avg.load_1_min, load_avg.load_5_min,
                        load_avg.load_15_min, system_memory_usage.free_ram,
                        process_memory_usage.virtual_memory, process_memory_usage.physical_memory});

  return output_table;
}

MetaSystemUtilizationTable::LoadAvg MetaSystemUtilizationTable::_get_load_avg() {
#if defined __linux__

  std::ifstream load_avg_file;
  load_avg_file.open("/proc/loadavg", std::ifstream::in);

  std::string load_avg_value;
  std::vector<float> load_avg_values;
  for (int value_index = 0; value_index < 3; ++value_index) {
    std::getline(load_avg_file, load_avg_value, ' ');
    load_avg_values.push_back(std::stof(load_avg_value));
  }
  load_avg_file.close();

  return {load_avg_values[0], load_avg_values[1], load_avg_values[2]};

#elif defined __APPLE__

  loadavg load_avg;
  size_t size = sizeof(load_avg);
  if (sysctlbyname("vm.loadavg", &load_avg, &size, nullptr, 0) != 0) {
    Fail("Unable to call sysctl vm.loadavg");
  }

  return {static_cast<float>(load_avg.ldavg[0]) / static_cast<float>(load_avg.fscale),
          static_cast<float>(load_avg.ldavg[1]) / static_cast<float>(load_avg.fscale),
          static_cast<float>(load_avg.ldavg[2]) / static_cast<float>(load_avg.fscale)};

#endif

  Fail("Method not implemented for this platform");
}

int MetaSystemUtilizationTable::_get_cpu_count() {
#if defined __linux__

  std::ifstream cpu_info_file;
  cpu_info_file.open("/proc/cpuinfo", std::ifstream::in);

  uint32_t processors = 0;
  for (std::string cpu_info_line; std::getline(cpu_info_file, cpu_info_line);) {
    if (cpu_info_line.rfind("processor", 0) == 0) ++processors;
  }

  cpu_info_file.close();

  return processors;

#elif defined __APPLE__

  uint32_t processors;
  size_t size = sizeof(processors);
  if (sysctlbyname("hw.ncpu", &processors, &size, nullptr, 0) != 0) {
    Fail("Unable to call sysctl hw.ncpu");
  }

  return processors;
#endif

  Fail("Method not implemented for this platform");
}

float MetaSystemUtilizationTable::_get_system_cpu_usage() {
#if defined __linux__

  static uint64_t last_user_time = 0u, last_user_nice_time = 0u, last_kernel_time = 0u, last_idle_time = 0u;

  std::ifstream stat_file;
  stat_file.open("/proc/stat", std::ifstream::in);

  std::string cpu_line;
  std::getline(stat_file, cpu_line);
  uint64_t user_time, user_nice_time, kernel_time, idle_time;
  std::sscanf(cpu_line.c_str(), "cpu %lu %lu %lu %lu", &user_time, &user_nice_time, &kernel_time, &idle_time);
  stat_file.close();

  auto used = (user_time - last_user_time) + (user_nice_time - last_user_nice_time) + (kernel_time - last_kernel_time);
  auto total = used + (idle_time - last_idle_time);

  last_user_time = user_time;
  last_user_nice_time = user_nice_time;
  last_kernel_time = kernel_time;
  last_idle_time = idle_time;

  auto cpus = _get_cpu_count();

  return (100.0 * used) / (total * cpus);

#elif defined __APPLE__

  static uint64_t last_total_ticks = 0u, last_idle_ticks = 0u;

  host_cpu_load_info_data_t cpu_info;
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu_info), &count) !=
      KERN_SUCCESS) {
    Fail("Unable to access host_statistics");
  }

  uint64_t total_ticks = 0;
  for (int cpu_state = 0; cpu_state <= CPU_STATE_MAX; ++cpu_state) {
    total_ticks += cpu_info.cpu_ticks[cpu_state];
  }
  auto idle_ticks = cpu_info.cpu_ticks[CPU_STATE_IDLE];

  auto total = total_ticks - last_total_ticks;
  auto idle = idle_ticks - last_idle_ticks;

  last_total_ticks = total_ticks;
  last_idle_ticks = idle_ticks;

  auto cpus = _get_cpu_count();

  return 100.0f * (1.0f - (static_cast<float>(idle) / static_cast<float>(total))) / cpus;

#endif

  Fail("Method not implemented for this platform");
}

float MetaSystemUtilizationTable::_get_process_cpu_usage() {
#if defined __linux__

  static clock_t last_clock_time = 0u, last_kernel_time = 0u, last_user_time = 0u;
  struct tms timeSample;

  auto clock_time = times(&timeSample);
  auto kernel_time = timeSample.tms_stime;
  auto user_time = timeSample.tms_utime;

  auto used = (user_time - last_user_time) + (kernel_time - last_kernel_time);
  auto total = clock_time - last_clock_time;

  last_user_time = user_time;
  last_kernel_time = kernel_time;
  last_clock_time = clock_time;

  auto cpus = _get_cpu_count();

  return (100.0 * used) / (total * cpus);

#elif defined __APPLE__

  static uint64_t last_clock_time = 0u, last_system_time = 0u, last_user_time = 0u;

  mach_timebase_info_data_t info;
  mach_timebase_info(&info);

  uint64_t clock_time = mach_absolute_time();

  struct rusage resource_usage;
  if (getrusage(RUSAGE_SELF, &resource_usage)) {
    Fail("Unable to access rusage");
  }

  uint32_t nano = 1'000'000'000, micro = 1'000;
  uint64_t system_time = resource_usage.ru_stime.tv_sec * nano + resource_usage.ru_stime.tv_usec * micro;
  uint64_t user_time = resource_usage.ru_utime.tv_sec * nano + resource_usage.ru_utime.tv_usec * micro;

  auto used = (user_time - last_user_time) + (system_time - last_system_time);
  auto total = (clock_time - last_clock_time) * info.numer / info.denom;

  last_clock_time = clock_time;
  last_user_time = user_time;
  last_system_time = system_time;

  return (100.0f * used) / (total);

#endif

  Fail("Method not implemented for this platform");
}

MetaSystemUtilizationTable::SystemMemoryUsage MetaSystemUtilizationTable::_get_system_memory_usage() {
#if defined __linux__

  struct sysinfo memory_info;
  sysinfo(&memory_info);

  MetaSystemUtilizationTable::SystemMemoryUsage memory_usage;
  memory_usage.total_ram = memory_info.totalram * memory_info.mem_unit;
  memory_usage.total_swap = memory_info.totalswap * memory_info.mem_unit;
  memory_usage.free_ram = memory_info.freeram * memory_info.mem_unit;
  memory_usage.free_swap = memory_info.freeswap * memory_info.mem_unit;
  memory_usage.total_memory = memory_usage.total_ram + memory_usage.total_swap;
  memory_usage.free_memory = memory_usage.free_ram + memory_usage.free_swap;

  return memory_usage;

#elif defined __APPLE__

  int64_t physical_memory;
  size_t size = sizeof(physical_memory);
  if (sysctlbyname("hw.memsize", &physical_memory, &size, nullptr, 0) != 0) {
    Fail("Unable to call sysctl hw.memsize");
  }

  // Attention: total swap might change if more swap is needed
  xsw_usage swap_usage;
  size = sizeof(swap_usage);
  if (sysctlbyname("vm.swapusage", &swap_usage, &size, nullptr, 0) != 0) {
    Fail("Unable to call sysctl vm.swapusage");
  }

  vm_size_t page_size;
  vm_statistics64_data_t vm_statistics;
  mach_msg_type_number_t count = sizeof(vm_statistics) / sizeof(natural_t);

  if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS ||
      host_statistics64(mach_host_self(), HOST_VM_INFO, reinterpret_cast<host_info64_t>(&vm_statistics), &count) !=
          KERN_SUCCESS) {
    Fail("Unable to access host_page_size or host_statistics64");
  }

  MetaSystemUtilizationTable::SystemMemoryUsage memory_usage;
  memory_usage.total_ram = physical_memory;
  memory_usage.total_swap = swap_usage.xsu_total;
  memory_usage.free_swap = swap_usage.xsu_avail;
  memory_usage.free_ram = vm_statistics.free_count * page_size;

  // auto used = (vm_statistics.active_couunt + vm_statistice.inactive_count + vm_statistics.wire_count) * page_size;

  return memory_usage;

#endif

  Fail("Method not implemented for this platform");
}

#if defined __linux__
int64_t MetaSystemUtilizationTable::_int_from_string(std::string input_string) {
  size_t index = 0;
  size_t begin = 0, end = input_string.length() - 1;

  for (; index < input_string.length(); ++index) {
    if (isdigit(input_string[index])) {
      begin = index;
      break;
    }
  }
  for (; index < input_string.length(); ++index) {
    if (!isdigit(input_string[index])) {
      end = index;
      break;
    }
  }
  return std::stol(input_string.substr(begin, end - begin));
}
#endif

MetaSystemUtilizationTable::ProcessMemoryUsage MetaSystemUtilizationTable::_get_process_memory_usage() {
#if defined __linux__

  std::ifstream self_status_file;
  self_status_file.open("/proc/self/status", std::ifstream::in);

  MetaSystemUtilizationTable::ProcessMemoryUsage memory_usage;
  for (std::string self_status_line; std::getline(self_status_file, self_status_line);) {
    if (self_status_line.rfind("VmSize", 0) == 0) {
      memory_usage.virtual_memory = _int_from_string(self_status_line) * 1000;
    } else if (self_status_line.rfind("VmRSS", 0) == 0) {
      memory_usage.physical_memory = _int_from_string(self_status_line) * 1000;
    }
  }

  self_status_file.close();

  return memory_usage;

#elif defined __APPLE__

  struct task_basic_info info;
  mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    Fail("Unable to access task_info");
  }

  return {static_cast<int64_t>(info.virtual_size), static_cast<int64_t>(info.resident_size)};

#endif

  Fail("Method not implemented for this platform");
}

}  // namespace opossum