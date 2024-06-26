/*
 * rdma.hpp/cpp:
 *   A wrapper organizing ibv_context, ibv_pd, ibv_cq, ibv_mr, ibv_qp and some other customized data structure for a RDMA RC connection together.
 *
 * How to use:
 *    1. auto [rdma, status] = RDMA::make_rdma()
 *    2. auto status = rdma->open()
 *    3. rdma->exchange_certificate(socket)
 *    4. rdma->modify_qp(init_attr, init_mask)
 *    5. rdma->modify_qp(rtr_attr, rtr_mask)
 *    6. rdma->modify_qp(rts_attr, rts_mask)
 *    7. rdma->post_send/recv/read/write
 *    8. rdma->poll_complection
 */
#ifndef __HILL__RDMA__RDMA__
#define __HILL__RDMA__RDMA__
#include "config/config.hpp"

#include <memory>
#include <optional>
#include <functional>
#include <iostream>
#include <cstring>

#include <infiniband/verbs.h>
#include <byteswap.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

namespace Hill {
    namespace RDMAUtil {
        struct connection_certificate {
            uint64_t addr;   // registered memory address
            uint32_t rkey;   // remote key
            uint32_t qp_num; // local queue pair number
            uint16_t lid;    // LID of the ib port
            uint8_t gid[16]; // mandatory for RoCE
        } __attribute__((packed));

        namespace Enums {
            enum class Status {
                Ok,
                NoRDMADeviceList,
                DeviceNotFound,
                DeviceNotOpened,
                NoGID,
                CannotOpenDevice,
                CannotAllocPD,
                CannotCreateCQ,
                CannotRegMR,
                CannotCreateQP,
                CannotQueryPort,
                InvalidGIDIdx,
                InvalidIBPort,
                InvalidArguments,
                CannotInitQP,
                QPRTRFailed,
                QPRTSFailed,
                ReadError,
                WriteError,
                PostFailed,
                RecvFailed,
            };
        }
        using namespace Enums;
        using StatusPair = std::pair<Enums::Status, int>;
        using byte_t = uint8_t;
        using byte_ptr_t = uint8_t *;
        using const_byte_ptr_t = const uint8_t *;
        auto decode_rdma_status(const Enums::Status& status) -> std::string;

        class RDMADevice;
        // Aggregation of pointers to ibv_context, ibv_pd, ibv_cq, ibv_mr and ibv_qp, which are used for further operations
        struct RDMAContext {
            struct ibv_context *ctx;
            struct ibv_pd *pd;
            struct ibv_cq *out_cq;
            struct ibv_cq *in_cq;            
            struct ibv_mr *mr;
            struct ibv_qp *qp;
            connection_certificate local, remote;
            void *buf;
            RDMADevice *device;
            
            auto post_send_helper(const uint8_t *msg, size_t msg_len, enum ibv_wr_opcode opcode, size_t local_offset,
                                  size_t remote_offset) -> StatusPair;
            auto post_send_helper(const byte_ptr_t &ptr, const uint8_t *msg, size_t msg_len, enum ibv_wr_opcode opcode,
                                  size_t local_offset) -> StatusPair;


            RDMAContext() = default;
            RDMAContext(const RDMAContext &) = delete;
            RDMAContext(RDMAContext &&) = delete;
            auto operator=(const RDMAContext &) = delete;
            auto operator=(RDMAContext &&) = delete;

            inline static auto make_rdma_context() -> std::unique_ptr<RDMAContext> {
                auto ret = std::make_unique<RDMAContext>();
                memset(ret.get(), 0, sizeof(RDMAContext));
                return ret;
            }
            
            ~RDMAContext() {
                if (qp) ibv_destroy_qp(qp);
                if (mr) ibv_dereg_mr(mr);
                if (out_cq) ibv_destroy_cq(out_cq);
                if (in_cq) ibv_destroy_cq(in_cq);                
                if (pd) ibv_dealloc_pd(pd);
                // do not release the ctx because it's shared by multiple RDMAContext instances
            }

            auto default_connect(int socket) -> int;

            auto modify_qp(struct ibv_qp_attr &, int mask) noexcept -> StatusPair;
            auto exchange_certificate(int sockfd) noexcept -> Status;

            auto post_send(const uint8_t *msg, size_t msg_len, size_t local_offset = 0)
                -> StatusPair;
            auto post_send(const byte_ptr_t &ptr, uint8_t *msg, size_t msg_len, size_t local_offset = 0)
                -> StatusPair;

            auto post_read(size_t msg_len, size_t local_offset = 0, size_t remote_offset = 0) -> StatusPair;
            auto post_read(const byte_ptr_t &ptr, size_t msg_len, size_t local_offset = 0) -> StatusPair;

            auto post_write(const uint8_t *msg, size_t msg_len, size_t local_offset = 0, size_t remote_offset = 0)
                -> StatusPair;
            auto post_write(const byte_ptr_t &ptr, const uint8_t *msg, size_t msg_len, size_t local_offset = 0)
                -> StatusPair;

            auto post_recv_to(size_t msg_len, size_t offset = 0) -> StatusPair;

            /*
             * A set of poll_completion functions. 
             * poll_completion_once(): just to check if a completion is generated
             * poll_one_completion(): poll if one completion is generated and return a ibv_wc struct or an error
             */
            auto poll_completion_once(bool send = true) noexcept -> int;
            auto poll_one_completion(bool send = true) noexcept -> std::pair<std::unique_ptr<struct ibv_wc>, int>;
            auto poll_multiple_completions(size_t no, bool send = true) noexcept
                -> std::pair<std::unique_ptr<struct ibv_wc[]>, int>;
            
            auto fill_buf(uint8_t *msg, size_t msg_len, size_t offset = 0) -> void;

            inline auto get_buf() const noexcept -> const void * {
                return buf;
            }

            inline auto get_char_buf() const noexcept -> const char * {
                return (char *)buf;
            }
        };

        /*
         * A wrapping class presenting a single RDMA device, all qps are created from a device should be 
         * created by invoking RDMADevice::open()
         */
        class RDMADevice {
        private:
            std::string dev_name;
            struct ibv_device **devices;
            struct ibv_device *device;
            struct ibv_context *ctx;
            int ib_port;
            int gid_idx;

        public:
            static auto make_rdma(const std::string &dev_name, int ib_port, int gid_idx)
                -> std::pair<std::unique_ptr<RDMADevice>, Status>
            {
                int dev_num = 0;
                auto ret = std::make_unique<RDMADevice>();
                ret->dev_name = dev_name;
                ret->ib_port = ib_port;
                ret->gid_idx = gid_idx;
                
                ret->devices = ibv_get_device_list(&dev_num);
                if (!ret->devices) {
                    return std::make_pair(nullptr, Status::NoRDMADeviceList);
                }

                for (int i = 0; i < dev_num; i++) {
                    if (dev_name.compare(ibv_get_device_name(ret->devices[i])) == 0) {
                        if (auto ctx = ibv_open_device(ret->devices[i]); ctx) {
                            ret->device = ret->devices[i];
                            ret->ctx = ctx;
                            return std::make_pair(std::move(ret), Status::Ok);
                        }
                    }
                }
                
                return std::make_pair(nullptr, Status::DeviceNotFound);
            }

            // never explicitly instantiated
            RDMADevice() = default;
            ~RDMADevice() {
                ibv_free_device_list(devices);
                ibv_close_device(ctx);
            }
            RDMADevice(const RDMADevice &) = delete;
            RDMADevice(RDMADevice &&) = delete;
            RDMADevice &operator=(const RDMADevice &) = delete;
            RDMADevice &operator=(RDMADevice&&) = delete;


            inline auto get_ib_port() const noexcept -> const int & {
                return ib_port;
            }

            inline auto get_gid_idx() const noexcept -> const int & {
                return gid_idx;
            }

            inline auto get_dev_name() const noexcept -> const std::string & {
                return dev_name;
            }

            /*
              Open an initialized RDMA device made from `make_rdma`
              @membuf: memory region to be registered
              @memsize: memory region size
              @cqe: completion queue capacity
              @attr: queue pair initialization attribute.
              No need to fill the `send_cq` and `recv_cq` fields, they are filled automatically
            */
            auto open(void *membuf, size_t memsize, size_t cqe, int mr_access, struct ibv_qp_init_attr &attr)
                -> std::pair<std::unique_ptr<RDMAContext>, Status>;

            inline static auto get_default_mr_access() -> int {
                return IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
            }

            static auto get_default_qp_init_attr() -> std::unique_ptr<struct ibv_qp_init_attr>;

            static auto get_default_qp_init_state_attr(const int ib_port = 1)
                -> std::unique_ptr<struct ibv_qp_attr>;
            inline static auto get_default_qp_init_state_attr_mask() -> int {
                return IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
            }

            static auto get_default_qp_rtr_attr(const connection_certificate &remote,
                                                const int ib_port,
                                                const int sgid_idx) -> std::unique_ptr<struct ibv_qp_attr>;
            static auto get_default_qp_rtr_attr_mask() -> int {
                return IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
            }

            static auto get_default_qp_rts_attr() -> std::unique_ptr<struct ibv_qp_attr>;
            static auto get_default_qp_rts_attr_mask() -> int {
                return IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                    IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
            }
        };
    }
}
#endif
