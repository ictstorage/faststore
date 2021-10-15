#ifndef __HILL__WORKLOAD__WORKLOAD__
#define __HILL__WORKLOAD__WORKLOAD__
#include <string>
#include <vector>
#include <iostream>
namespace Hill {
    namespace Workload {
        namespace Enums {
            enum class WorkloadType {
                Insert,
                Update,
                Delete,
                Search,
                Range,
                Unknownk,
            };
        }

        struct WorkloadItem {
            Enums::WorkloadType type;
            std::string key;
            std::string key_or_value;

            WorkloadItem();
            WorkloadItem(const WorkloadItem &r) = default;
            WorkloadItem(WorkloadItem &&) = default;
            auto operator=(const WorkloadItem &) -> WorkloadItem & = default;
            auto operator=(WorkloadItem &&) -> WorkloadItem & = default;
            
            static auto make_workload_item(const Enums::WorkloadType &type, const std::string &key) -> WorkloadItem {
                if (type != Enums::WorkloadType::Search) {
                    throw std::invalid_argument("WorkloadItem should be search");
                }
                
                WorkloadItem item;
                item.type = type;
                item.key = key;
                return item;
            }

            static auto make_workload_item(const Enums::WorkloadType &type, const std::string &key, const std::string &key_or_value) -> WorkloadItem {
                if (type == Enums::WorkloadType::Search) {
                    throw std::invalid_argument("WorkloadItem should be search");
                }
                
                WorkloadItem item;
                item.type = type;
                item.key = key;
                item.key_or_value = key_or_value;
                return item;
            }
        };

        using StringWorkload = std::vector<WorkloadItem>;
    }
}
#endif