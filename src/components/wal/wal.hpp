#ifndef __HILL__WAL__WAL__
#define __HILL__WAL__WAL__

#include "memory_manager/memory_manager.hpp"

#include <memory>
#include <functional>
#include <unordered_set>


namespace Hill {
    using namespace Memory::TypeAliases;    
    namespace WAL {
        namespace Constants {
            static constexpr int iREGION_NUM = Hill::Memory::Constants::iTHREAD_LIST_NUM;
            static constexpr size_t uBATCH_SIZE = 1024UL;
            static constexpr size_t uREGION_SIZE = 1024UL;
            static constexpr uint64_t uLOG_REGIONS_MAGIC = 0x1357246813572468UL;
        }

        namespace Enums {
            enum class LogStatus {
                Uncommited,
                Committed,
                None,
            };

            enum class LoggerRecoverStatus {
                Ok,
                NoLogger,
            };

            enum class RegionsRecoverStatus {
                Ok,
                NoRegions,
            };
            
            enum class RegionRecoverStatus {
                Ok,
                No,
            };

            enum class Ops {
                Insert,
                Update,                
                Delete,
                Unknown,
            };
        }

        struct LogEntry {
            byte_ptr_t address;
            Enums::Ops op;
            Enums::LogStatus status;

            LogEntry() : address(nullptr), op(Enums::Ops::Unknown),status(Enums::LogStatus::None) {};

            static auto make_entry(const byte_ptr_t &ptr) -> LogEntry & {
                auto tmp = reinterpret_cast<LogEntry *>(ptr);
                tmp->address = nullptr;
                tmp->op = Enums::Ops::Unknown;
                tmp->status = Enums::LogStatus::None;
                return *tmp;
            }
            ~LogEntry() = default;
            LogEntry(const LogEntry &) = delete;
            LogEntry(LogEntry &&) = delete;
            auto operator=(const LogEntry &) -> LogEntry & = delete;
            auto operator=(LogEntry &&) -> LogEntry & = delete;            
            
        };

        using LogEntryAction = std::function<bool(LogEntry &)>;

        struct LogRegion {
            size_t checkpointed;
            size_t cursor;
            LogEntry entries[Constants::uBATCH_SIZE * Constants::uREGION_SIZE];

            static auto make_region(const byte_ptr_t &ptr) -> LogRegion & {
                auto tmp = reinterpret_cast<LogRegion *>(ptr);

                for (auto &e : tmp->entries) {
                    LogEntry::make_entry(reinterpret_cast<byte_ptr_t>(&e));
                }
                tmp->checkpointed = 0;
                tmp->cursor = 0;
                return *tmp;
            }

            /*
             * Recover iterates over each uncheckpointed log entry and apply the 
             * user-defined callback to the entry
             *
             * During the iteration, memory chunks are logically reclaimed, contents
             * in the memory chunks are not touched, thus the callback is allowed 
             * to use the contents. The logically reclaimed memory chunks is allocated
             * upon allocation, thus once recovery is done, the contents are not 
             * guaranteed to be valid.
             */
            using page_vector_ptr = std::unique_ptr<std::vector<Memory::Page *>>;
            auto recover(LogEntryAction log_action) noexcept -> page_vector_ptr;

            LogRegion() = delete;
            ~LogRegion() = default;
            LogRegion(const LogRegion &) = delete;
            LogRegion(LogRegion &&) = delete; 
            auto operator=(const LogRegion &) -> LogRegion & = delete;
            auto operator=(LogRegion &&) -> LogRegion & = delete;

        private:
            auto recover_op(LogEntry &, std::unordered_set<Memory::Page *> &, page_vector_ptr &) noexcept -> LogEntry &;
            auto recover_page(Memory::Page *) noexcept -> std::optional<Memory::Page *>;
        };

        struct LogRegions {
            size_t magic;
            LogRegion regions[Constants::iREGION_NUM];

            static auto make_regions(const byte_ptr_t &ptr) -> LogRegions & {
                auto tmp = reinterpret_cast<LogRegions *>(ptr);

                for (int i = 0; i < Constants::iREGION_NUM; i++) {
                    LogRegion::make_region(reinterpret_cast<byte_ptr_t>(&tmp->regions[i]));
                }

                tmp->magic = Constants::uLOG_REGIONS_MAGIC;
                return *tmp;
            }

            static auto recover_regions(const byte_ptr_t &ptr, LogEntryAction action) noexcept -> LogRegions & {
                auto tmp = reinterpret_cast<LogRegions *>(ptr);

                if (tmp->magic == Constants::uLOG_REGIONS_MAGIC) {
                    for (int i = 0; i < Constants::iREGION_NUM; i++) {
                        tmp->regions[i].recover(action);
                    }
                }

                return make_regions(ptr);
            }
                
            LogRegions() = delete;
            ~LogRegions() = default;
            LogRegions(const LogRegions &) = delete;
            LogRegions(LogRegions &&) = delete; 
            auto operator=(const LogRegions &) -> LogRegions & = delete;
            auto operator=(LogRegions &&) -> LogRegions & = delete;
            
        };

        /*
         * !!! NEVER INHERIT FROM ANY OTHER CLASSES OR STRUCTS
         * WAL::logger is used in combination with Memory::MemoryManager to avoid memory leaks and 
         * redo/undo operations
         *
         * Upon recovery, each address should be checked, i.e., the page owning the the address. 
         * should be scanned to find the exact number of valid records. Since loggin entryies are 
         * committed in batches, there at most Constants::iREGION_NUM * Constants::uBATCH_SIZE
         * 
         */
        class Logger {
        public:
            static auto make_unique_logger(const byte_ptr_t &pm_ptr) -> std::unique_ptr<Logger> {
                auto out = std::make_unique<Logger>();
                out->regions = &LogRegions::make_regions(pm_ptr);
                out->init_utility();                

                return out;
            }

            static auto make_shared_logger(const byte_ptr_t &pm_ptr) -> std::shared_ptr<Logger> {
                auto out = std::make_shared<Logger>();
                out->regions = &LogRegions::make_regions(pm_ptr);
                out->init_utility();                
                
                return out;
            }

            static auto recover_unique_logger(const byte_ptr_t &pm_ptr, LogEntryAction action) -> std::unique_ptr<Logger> {
                auto out = std::make_unique<Logger>();
                out->regions = &LogRegions::recover_regions(pm_ptr, action);
                out->init_utility();

                return out;
            }

            static auto recover_shared_logger(const byte_ptr_t &pm_ptr, LogEntryAction action) -> std::shared_ptr<Logger> {
                auto out = std::make_shared<Logger>();
                out->regions = &LogRegions::recover_regions(pm_ptr, action);
                out->init_utility();
                
                return out;
            }
            

            auto register_thread() noexcept -> std::optional<int>;
            auto unregister_thread() noexcept -> void;
            
            Logger() = default;
            ~Logger() = default;
            Logger(const Logger &) = delete;
            Logger(Logger &&) = delete; 
            auto operator=(const Logger &) -> Logger & = delete;
            auto operator=(Logger &&) -> Logger & = delete;
            
        private:
            LogRegions *regions;
            bool in_use[Constants::iREGION_NUM];
            size_t counters[Constants::iREGION_NUM];

            auto init_utility() noexcept -> void {
                for (int i = 0; i < Constants::iREGION_NUM; i++) {
                    in_use[i] = 0;
                    counters[i] = 0;
                }
            }
        };
    }
}
#endif