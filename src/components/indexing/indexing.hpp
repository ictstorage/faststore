#ifndef __HILL__INDEXING__INDEXING__
#define __HILL__INDEXING__INDEXING__
#include "memory_manager/memory_manager.hpp"
#include "remote_memory/remote_memory.hpp"
#include "wal/wal.hpp"
#include "kv_pair/kv_pair.hpp"
#include "misc/misc.hpp"
#include "coloring/coloring.hpp"
#include "debug_logger/debug_logger.hpp"

#include <vector>
#include <atomic>
#include <cstring>

namespace Hill {
    namespace Indexing {
        using namespace Memory::TypeAliases;
        using namespace KVPair::TypeAliases;
        using namespace WAL::TypeAliases;
        namespace Constants {
#ifdef __HILL_DEBUG__
            static constexpr int iDEGREE = 3;
            static constexpr int iNUM_HIGHKEY = iDEGREE - 1;
#else
            static constexpr int iDEGREE = 64;
            static constexpr int iNUM_HIGHKEY = iDEGREE - 1;
#endif
        }

        namespace Enums {
            enum class OpStatus {
                Ok,
                Failed,
                Retry,
                NoMemory,
                NeedSplit,
                RepeatInsert,
            };

            enum class NodeType : byte_t {
                Leaf,
                Inner,
                Unknown,
            };
        }

        struct VersionLock {
            std::atomic<uint64_t> l;
            VersionLock() : l(0) {};
            ~VersionLock() = default;
            VersionLock(const VersionLock &) = delete;
            VersionLock(VersionLock &&) = delete;
            auto operator=(const VersionLock &) -> VersionLock = delete;
            auto operator=(VersionLock &&) -> VersionLock = delete;

            inline auto lock() noexcept -> void {
                auto expected = 0UL;
                uint64_t tmp;
                do {
                    tmp = l.load();
                    expected = tmp & (~0x1UL);
                } while (!l.compare_exchange_strong(expected, tmp | (0x1UL)));
            }

            inline auto try_lock() noexcept -> bool {
                auto tmp = l.load();
                auto expected = tmp & (~0x1UL);
                auto desired = tmp | (0x1UL);
                return l.compare_exchange_strong(expected, desired);
            }

            inline auto unlock() noexcept -> void {
                l += 1;
            }

            inline auto is_locked() const noexcept -> bool {
                return l.load() & (0x1UL);
            }

            inline auto version() const noexcept -> uint64_t {
                return l >> 1;
            }

            inline auto whole_value() const noexcept -> uint64_t {
                return l.load();
            }

            inline auto reset() noexcept -> void {
                l.store(0);
            }
        };

        struct InnerNode;
        struct LeafNode {
            InnerNode *parent;
            hill_key_t *highkey;
            hill_key_t *keys[Constants::iNUM_HIGHKEY];
            Memory::PolymorphicPointer values[Constants::iNUM_HIGHKEY];
            size_t value_sizes[Constants::iNUM_HIGHKEY];
            LeafNode *right_link;

            // for convenient access
            VersionLock version_lock;

            LeafNode() = delete;
            // All nodes are on PM, not in heap or stack
            ~LeafNode() = delete;
            LeafNode(const LeafNode &) = delete;
            LeafNode(LeafNode &&) = delete;
            auto operator=(const LeafNode &) = delete;
            auto operator=(LeafNode &&) = delete;

            static auto make_leaf(const byte_ptr_t &ptr) -> struct LeafNode * {
                auto tmp = reinterpret_cast<LeafNode *>(ptr);
                tmp->version_lock.reset();
                for (int i = 0; i < Constants::iNUM_HIGHKEY; i++) {
                    tmp->keys[i] = nullptr;
                    tmp->values[i] = nullptr;
                    tmp->value_sizes[i] = 0;
                }
                tmp->right_link = nullptr;
                tmp->highkey = nullptr;
                tmp->parent = nullptr;
                return tmp;
            }

            inline auto is_full() const noexcept -> bool {
                return keys[Constants::iNUM_HIGHKEY - 1] != nullptr;
            }

            inline auto lock() noexcept -> void {
                version_lock.lock();
            }

            inline auto unlock() noexcept -> void {
                version_lock.unlock();
            }

            inline auto try_lock() noexcept -> bool {
                return version_lock.try_lock();
            }

            inline auto version() const noexcept -> uint64_t {
                return version_lock.version();
            }

            inline auto is_locked() const noexcept -> bool {
                return version_lock.is_locked();
            }

            auto insert(int tid, WAL::Logger *log, Memory::Allocator *alloc, Memory::RemoteMemoryAgent *agent,
                        const char *k, size_t k_sz, const char *v, size_t v_sz) -> Enums::OpStatus;
            auto dump() const noexcept -> void;
        };


        struct PolymorphicNodePointer {
            Enums::NodeType type;
            void *value;
            PolymorphicNodePointer() : type(Enums::NodeType::Unknown), value(nullptr) {};
            PolymorphicNodePointer(std::nullptr_t nu) : type(Enums::NodeType::Unknown), value(nu) {};
            PolymorphicNodePointer(const PolymorphicNodePointer &) = default;
            PolymorphicNodePointer(LeafNode *l) : type(Enums::NodeType::Leaf), value(l) {};
            PolymorphicNodePointer(InnerNode *l) : type(Enums::NodeType::Inner), value(l) {};
            ~PolymorphicNodePointer() = default;
            auto operator=(const PolymorphicNodePointer &) -> PolymorphicNodePointer & = default;
            auto operator=(PolymorphicNodePointer &&) -> PolymorphicNodePointer & = default;
            auto operator=(std::nullptr_t nu) -> PolymorphicNodePointer & {
                type = Enums::NodeType::Unknown;
                value = nu;
                return *this;
            }
            auto operator=(LeafNode *v) -> PolymorphicNodePointer & {
                type = Enums::NodeType::Leaf;
                value = reinterpret_cast<void *>(v);
                return *this;
            }

            auto operator=(InnerNode *v) -> PolymorphicNodePointer & {
                type = Enums::NodeType::Inner;
                value = reinterpret_cast<void *>(v);
                return *this;
            }


            inline auto is_leaf() const noexcept -> bool {
                return type == Enums::NodeType::Leaf;
            }

            inline auto is_inner() const noexcept -> bool {
                return type == Enums::NodeType::Inner;
            }

            inline auto is_null() const noexcept -> bool {
                return value == nullptr;
            }

            template<typename T>
            inline auto get_as() const noexcept -> typename std::enable_if<std::is_same_v<T, LeafNode *>, LeafNode *>::type {
                return reinterpret_cast<T>(value);
            }

            template<typename T>
            inline auto get_as() const noexcept -> typename std::enable_if<std::is_same_v<T, InnerNode *>, InnerNode *>::type {
                return reinterpret_cast<T>(value);
            }

            inline auto get_highkey() const noexcept -> hill_key_t * {
                // InnerNode and LeafNode's memory layouts are similar
                return get_as<LeafNode *>()->highkey;
            }

            inline auto get_parent() const noexcept -> InnerNode * {
                return get_as<LeafNode *>()->parent;
            }

            inline auto set_parent(InnerNode *p) -> void {
                get_as<LeafNode *>()->parent = p;
            }
        };


        /*
         * The layout of a node is as follows
         * | k1 | k2 | k3 |
         * | c1 | c2 | c3 | c4 |
         * Each child stored keys <= highkey
         *
         * Note we do not keep a parent pointer because a vector is used for backtracing
         * We do not use smart pointers either because we need atomic update to pointers
         */
        struct InnerNode {
            InnerNode *parent;
            hill_key_t *highkey;
            hill_key_t *keys[Constants::iNUM_HIGHKEY];
            PolymorphicNodePointer children[Constants::iDEGREE];
            InnerNode *right_link;
            VersionLock version_lock;

            InnerNode() = default;
            // All nodes are on PM, not in heap or stack
            ~InnerNode() = default;
            InnerNode(const InnerNode &) = delete;
            InnerNode(InnerNode &&) = delete;
            auto operator=(const InnerNode &) = delete;
            auto operator=(InnerNode &&) = delete;

            static auto make_inner() -> InnerNode * {
                auto tmp = new InnerNode;
                tmp->version_lock.reset();
                for (int i = 0; i < Constants::iNUM_HIGHKEY; i++) {
                    tmp->keys[i] = nullptr;
                    tmp->children[i] = nullptr;
                }
                tmp->right_link = nullptr;
                tmp->highkey = nullptr;
                tmp->parent = nullptr;
                tmp->children[Constants::iDEGREE - 1] = nullptr;
                return tmp;
            }

            inline auto is_full() const noexcept -> bool {
                return keys[Constants::iNUM_HIGHKEY - 1] != nullptr;
            }

            inline auto lock() noexcept -> void {
                version_lock.lock();
            }

            inline auto unlock() noexcept -> void {
                version_lock.unlock();
            }

            inline auto try_lock() noexcept -> bool {
                return version_lock.try_lock();
            }

            inline auto version() const noexcept -> uint64_t {
                return version_lock.version();
            }

            inline auto is_locked() const noexcept -> bool {
                return version_lock.is_locked();
            }

            // this child should be on the right of split_key
            auto insert(const hill_key_t *split_key, PolymorphicNodePointer child) -> Enums::OpStatus;
            auto dump() const noexcept -> void;
        };

        class OLFIT {
        public:
            // for convenience of testing
            OLFIT(int tid, Memory::Allocator *alloc_, WAL::Logger *logger_)
                : root(nullptr), alloc(alloc_), logger(logger_), agent(nullptr) {
                // NodeSplit is also for new root node creation
                auto ptr = logger->make_log(tid, WAL::Enums::Ops::NodeSplit);
                // crashing here is ok, because no memory allocation is done;
                alloc->allocate(tid, sizeof(LeafNode), ptr);
                /*
                 * crash here is ok, allocation is done. Crash in the allocation function
                 * is fine because on recovery, the allocator scans memory regions to restore
                 * partially allocated memory blocks
                 */
                root = LeafNode::make_leaf(ptr);
                logger->commit(tid);
                debug_logger = DebugLogger::MultithreadLogger::make_logger();
            }
            ~OLFIT() = default;

            static auto make_olfit(Memory::Allocator *alloc, WAL::Logger *logger) -> std::unique_ptr<OLFIT> {
#ifdef __HILL_INFO__
                std::cout << ">> OLFIT degree: " << Constants::iDEGREE << "\n";
#endif

                auto a_tid = alloc->register_thread();
                if (!a_tid.has_value()) {
                    return nullptr;
                }

                auto l_tid = logger->register_thread();
                if (!l_tid.has_value()) {
                    return nullptr;
                }

                if (a_tid.value() != l_tid.value()) {
                    alloc->unregister_thread(a_tid.value());
                    logger->unregister_thread(l_tid.value());
                    return nullptr;
                }
                auto ret = std::make_unique<OLFIT>(a_tid.value(), alloc, logger);
                alloc->unregister_thread(a_tid.value());
                logger->unregister_thread(l_tid.value());
                return ret;
            }

            // external interfaces use const char * as input
            auto insert(int tid, const char *k, size_t k_sz, const char *v, size_t v_sz) noexcept -> Enums::OpStatus;
            auto search(const char *k, size_t k_sz) const noexcept -> std::pair<Memory::PolymorphicPointer, size_t>;
            inline auto enable_agent(Memory::RemoteMemoryAgent *agent_) -> void {
                agent = agent_;
            }
            inline auto open_log(const std::string &log_file) -> bool {
                return debug_logger->open_log(log_file);
            }
            auto dump() const noexcept -> void;

        private:
            PolymorphicNodePointer root;
            Memory::Allocator *alloc;
            WAL::Logger *logger;
            Memory::RemoteMemoryAgent *agent;
            std::unique_ptr<DebugLogger::MultithreadLogger> debug_logger;

            auto traverse_node(const char *k, size_t k_sz) const noexcept -> LeafNode * {
                std::stringstream ss;
                if (root.is_leaf()) {
                    ss << "Root located " << root.get_as<LeafNode *>();
                    debug_logger->log_info(ss.str());
                    return root.get_as<LeafNode *>();
                }

                PolymorphicNodePointer current = root;
                PolymorphicNodePointer next = nullptr;
                InnerNode *inner;
                auto version = 0UL;
                while (!current.is_leaf()) {
                    inner = current.get_as<InnerNode *>();
                    ss << "Finding " << inner << " with ";
                    for (int i = 0; i < Constants::iNUM_HIGHKEY; i++) {
                        if (inner->keys[i]) {
                            ss << inner->keys[i]->to_string() << " ";
                        }
                    }
                    ss << "and highkey " << inner->highkey->to_string();
                    debug_logger->log_info(ss.str());
                    ss.str("");
                    version = inner->version_lock.version();
                    next = find_next(inner, k, k_sz);
                    if (inner->version_lock.version() == version) {
                        current = next;
                    }
                }
                ss << "Finding " << current.get_as<LeafNode *>() << " with ";
                for (int i = 0; i < Constants::iNUM_HIGHKEY; i++) {
                    if (current.get_as<LeafNode *>()->keys[i]) {
                        ss << current.get_as<LeafNode *>()->keys[i]->to_string() << " ";
                    }
                }

                ss << "and highkey " << current.get_as<LeafNode *>()->highkey->to_string();
                debug_logger->log_info(ss.str());
                return current.get_as<LeafNode *>();
            }

            auto traverse_node_no_tracing(const char *k, size_t k_sz) const noexcept -> LeafNode * {
                if (root.is_leaf()) {
                    return root.get_as<LeafNode *>();
                }

                PolymorphicNodePointer current = root;
                PolymorphicNodePointer next = nullptr;
                InnerNode *inner;
                auto version = 0UL;
                while (!current.is_leaf()) {
                    inner = current.get_as<InnerNode *>();
                    version = inner->version_lock.version();
                    next = find_next_no_tracing(inner, k, k_sz);
                    if (inner->version_lock.version() == version) {
                        current = next;
                    }
                }
                return current.get_as<LeafNode *>();
            }


            // follow the original paper of OLFIT, OT
            auto find_next(InnerNode *current, const char *k, size_t k_sz) const noexcept -> PolymorphicNodePointer {
                auto result = current->highkey->compare(k, k_sz);
                if (result == 0) {
                    int i;
                    for (i = Constants::iDEGREE - 1; i >= 0; i--) {
                        if (!current->children[i].is_null()) {
                            break;
                        }
                    }
                    return current->children[i];
                } else if (result > 0) {
                    int i;
                    for (i = 0; i < Constants::iNUM_HIGHKEY; i++) {
                        if (current->keys[i] == nullptr || current->keys[i]->compare(k, k_sz) > 0) {
                            return current->children[i];
                        }
                    }
                    return current->children[i];
                } else {
                    if (current->right_link) {
                        return current->right_link;
                    } else {
                        int i;
                        for (i = Constants::iDEGREE - 1; i >= 0; i--) {
                            if (!current->children[i].is_null()) {
                                break;
                            }
                        }
                        return current->children[i];
                    }
                }
            }

            // copied code above here, just to escape redundent branches
            auto find_next_no_tracing(InnerNode *current, const char *k, size_t k_sz) const noexcept -> PolymorphicNodePointer {
                auto result = current->highkey->compare(k, k_sz);
                if (result == 0) {
                    int i;
                    for (i = Constants::iDEGREE - 1; i >= 0; i--) {
                        if (!current->children[i].is_null()) {
                            break;
                        }
                    }
                    return current->children[i];
                } else if (result > 0) {
                    int i;
                    for (i = 0; i < Constants::iNUM_HIGHKEY; i++) {
                        if (current->keys[i] == nullptr || current->keys[i]->compare(k, k_sz) > 0) {
                            return current->children[i];
                        }
                    }
                    return current->children[i];
                } else {
                    if (current->right_link) {
                        return current->right_link;
                    } else {
                        int i;
                        for (i = Constants::iDEGREE - 1; i >= 0; i--) {
                            if (!current->children[i].is_null()) {
                                break;
                            }
                        }
                        return current->children[i];
                    }
                }
            }

            auto move_right(LeafNode *leaf, const char *k, size_t k_sz) -> LeafNode * {
                // leaf-hightkey == nullptr is true on start
                std::stringstream ss;
                ss << "Checking leaf node " << leaf << " with ";
                for (int i = 0; i < Constants::iNUM_HIGHKEY; i++) {
                    if (leaf->keys[i]) {
                        ss << leaf->keys[i]->to_string() << " ";
                    }
                }
                ss << "and highkey " << (leaf->highkey ? leaf->highkey->to_string() : " nullptr ");
                debug_logger->log_info(ss.str());
                if (!leaf->right_link) {
                    return leaf;
                }

                if (leaf->right_link->keys[0]->compare(k, k_sz) > 0) {
                    return leaf;
                }
                leaf->right_link->lock();
                leaf->unlock();
                return move_right(leaf->right_link, k, k_sz);
            }

            auto update_highkeys(LeafNode *leaf) -> void {
                std::stringstream ss;
                if (!leaf->parent) {
                    ss << "Leaf " << leaf << " has no parent";
                    debug_logger->log_info(ss.str());
                    ss.str("");
                    return;
                }

                PolymorphicNodePointer current = leaf;
                auto parent = leaf->parent;

                do {
                    int i;
                    for (i = Constants::iDEGREE - 1; i >= 0; i--) {
                        if (!parent->children[i].is_null()) {
                            break;
                        }
                    }

                    if (parent->children[i].value != current.value) {
                        return;
                    }
                    
                    parent->lock();
                    if (parent == current.get_parent()) {
                        ss << "Updating parent " << parent << "\\'s highkey to be " << current.get_highkey()->to_string();
                        debug_logger->log_info(ss.str());
                        ss.str("");
                        parent->highkey = current.get_highkey();
                    }
                    parent->unlock();
                    current = parent;
                    parent = current.get_parent();
                } while (parent);
            }

            // split an old node and return a new node with keys migrated
            auto split_leaf(int tid, LeafNode *l, const char *k, size_t k_sz, const char *v, size_t v_sz) -> LeafNode *;
            // split_inner is seperated from split leaf because they have different memory policies
            auto split_inner(InnerNode *l, const hill_key_t *splitkey, PolymorphicNodePointer child) -> std::pair<InnerNode *, hill_key_t *>;
            // push up split keys to ancestors
            auto push_up(LeafNode *new_leaf) -> Enums::OpStatus;
        };
    }
}
#endif
