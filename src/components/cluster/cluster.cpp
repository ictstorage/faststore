#include "cluster.hpp"


#include <iostream>
#include <fstream>
#include <fcntl.h>

namespace Hill {
    namespace Cluster {
        auto RangeGroup::add_main(const std::string &s, int node_id) -> void {
            if (node_id == 0) {
                std::cerr << ">> Error: node 0 is not supposed to be in range group\n";
                throw std::invalid_argument("node_id == 0 in add_main is not valid");
            }

            for (size_t i = 0; i < num_infos; i++) {
                if (infos[i].start == s) {
                    // duplicated, a main server already exists
                    std::cerr << ">> Warning: duplicated main server\n";
                    return;
                }
            }

            auto buf = new RangeInfo[num_infos + 1];
            for (size_t i = 0; i < num_infos; i++) {
                buf[i] = infos[i];
            }

            infos.reset(buf);
            infos[num_infos].nodes[0] = node_id;
            infos[num_infos].is_mem[0] = false;
            infos[num_infos].start = s;
            ++num_infos;
        }

        auto RangeGroup::append_node(const std::string &s, int node_id, bool is_mem) -> void {
            if (node_id == 0) {
                std::cerr << ">> Error: node 0 is not supposed to be in range group\n";
                throw std::invalid_argument("node_id == 0 in add_main is not valid");
            }

            if (num_infos == 0) {
                std::cerr << ">> Error: add a main server first\n";
                return;
            }

            for (size_t i = 0; i < num_infos; i++) {
                if (infos[i].start == s) {
                    if (infos[i].nodes[node_id] != 0) {
                        return;
                    }

                    // this duplication is just for convenience that node[0] = main server's node_id
                    infos[i].nodes[node_id] = node_id;
                    infos[i].is_mem[node_id] = is_mem;
                    return;
                }
            }

            std::cerr << ">> Error: no main server found\n";
        }

        auto RangeGroup::append_cpu(const std::string &s, int node_id) noexcept -> void {
            append_node(s, node_id, false);
        }

        auto RangeGroup::append_mem(const std::string &s, int node_id) noexcept -> void {
            append_node(s, node_id, true);
        }

        /*
         * The protocol buffer if in following format
         * -------  Fixed Field  -------
         * 8B             |    node_num
         * sizeof(nodes)  |    nodes
         * 8B             |    num_infos
         * ------- Dynamic Field -------
         * 8B             |  string size
         * start.size()   |  string
         * sizeof(is_mem) |  is_mem
         * sizeof(nodes)  |  nodes
         */
        auto ClusterMeta::total_size() const noexcept -> size_t {
            // node_num + nodes
            auto total_size = sizeof(cluster) + sizeof(version);
            // num_infos
            total_size += sizeof(group.num_infos);
            // dynamic field
            for (size_t i = 0; i < group.num_infos; i++) {
                total_size += sizeof(version);
                total_size += sizeof(uint64_t);
                total_size += group.infos[i].start.size();
                total_size += sizeof(group.infos[i].is_mem);
                total_size += sizeof(group.infos[i].nodes);
            }
            return total_size;
        }

        auto ClusterMeta::serialize() const noexcept -> std::unique_ptr<byte_t[]> {
            auto buf = new byte_t[total_size()];
            auto offset = 0UL;
            // all our machines are little-endian, no need to convert
            // I separate these fields just for easy debugging
            memcpy(buf, &version, sizeof(version));
            offset += sizeof(version);
            memcpy(buf + offset, &cluster.node_num, sizeof(cluster.node_num));
            offset += sizeof(cluster.node_num);
            memcpy(buf + offset, &cluster.nodes, sizeof(cluster.nodes));
            offset += sizeof(cluster.nodes);
            memcpy(buf + offset, &group.num_infos, sizeof(group.num_infos));
            offset += sizeof(group.num_infos);
            for (size_t i = 0; i < group.num_infos; i++) {
                memcpy(buf + offset, &group.infos[i].version, sizeof(group.infos[i].version));
                offset += sizeof(group.infos[i].version);
                auto tmp = group.infos[i].start.size();
                // header
                memcpy(buf + offset, &tmp, sizeof(tmp));
                offset += sizeof(tmp);
                // string
                memcpy(buf + offset, group.infos[i].start.data(), group.infos[i].start.size());
                offset += group.infos[i].start.size();
                // is_mem
                memcpy(buf + offset, group.infos[i].is_mem, sizeof(group.infos[i].is_mem));
                offset += sizeof(group.infos[i].is_mem);
                // nodes
                memcpy(buf + offset, group.infos[i].nodes, sizeof(group.infos[i].nodes));
                offset += sizeof(group.infos[i].nodes);
            }

            return std::unique_ptr<byte_t[]>(buf);
            // return buf;
        }

        auto ClusterMeta::deserialize(const byte_t *buf) -> void {
            auto offset = 0UL;
            memcpy(&version, buf, sizeof(version));
            offset += sizeof(version);
            memcpy(&cluster.node_num, buf + offset, sizeof(cluster.node_num));
            offset += sizeof(cluster.node_num);
            memcpy(&cluster.nodes, buf + offset, sizeof(cluster.nodes));
            offset += sizeof(cluster.nodes);
            memcpy(&group.num_infos, buf + offset, sizeof(group.num_infos));
            offset += sizeof(group.num_infos);
            auto infos = new RangeInfo[group.num_infos];
            for (size_t i = 0; i < group.num_infos; i++) {
                memcpy(&infos[i].version, buf + offset, sizeof(infos[i].version));
                buf += sizeof(infos[i].version);
                auto tmp = 0ULL;
                memcpy(&tmp, buf + offset, sizeof(tmp));
                offset += sizeof(tmp);
                infos[i].start.assign(reinterpret_cast<const char *>(buf + offset), tmp);
                offset += tmp;
                memcpy(infos[i].is_mem, buf + offset, sizeof(infos[i].is_mem));
                offset += sizeof(infos[i].is_mem);
                memcpy(infos[i].nodes, buf + offset, sizeof(infos[i].nodes));
                offset += sizeof(infos[i].nodes);
            }
            group.infos.reset(infos);
        }

        auto ClusterMeta::update(const ClusterMeta &newer) -> void {
            {
                std::scoped_lock l(lock);
                if (version >= newer.version) {
                    return;
                } else {
                    version = newer.version;
                }
                for (size_t i = 0; i < Constants::uMAX_NODE; i++) {
                    if (cluster.nodes[i].version < newer.cluster.nodes[i].version) {
                        cluster.nodes[i] = newer.cluster.nodes[i];
                    }
                }

                /*
                 * This update is not always correct because range group may change, e.g., more partitions are
                 * created. But currently I don't handle this because for experiment, range group is fixed
                 *
                 * To fully update a range group, we can make use RPC.
                 */
                for (size_t i = 0; i < newer.group.num_infos; i++) {
                    // order of RangeInfo never changes in a range group
                    if (group.infos[i].version < newer.group.infos[i].version) {
                        group.infos[i].version = newer.group.infos[i].version;
                        memcpy(group.infos[i].nodes, newer.group.infos[i].nodes, sizeof(group.infos[i].nodes));
                        memcpy(group.infos[i].is_mem, newer.group.infos[i].is_mem, sizeof(group.infos[i].is_mem));
                    }
                }
            }
        }

        auto ClusterMeta::dump() const noexcept -> void {
            std::cout << "--------------------- Meta Info --------------------- \n";
            std::cout << ">> version: " << version << "\n";
            std::cout << ">> node num: " << cluster.node_num << "\n";
            std::cout << ">> node info: \n";
            for (size_t i = 0; i < Constants::uMAX_NODE; i++) {
                if (cluster.nodes[i].node_id == 0)
                    continue;
                std::cout << ">> node " << i << "\n";
                std::cout << "-->> version: " << cluster.nodes[i].version << "\n";
                std::cout << "-->> node id: " << cluster.nodes[i].node_id << "\n";
                std::cout << "-->> total pm: " << cluster.nodes[i].total_pm << "\n";
                std::cout << "-->> availabel pm: " << cluster.nodes[i].available_pm << "\n";
                std::cout << "-->> ip address: " << cluster.nodes[i].addr.to_string() << "\n";
                std::cout << "-->> socket port: " << cluster.nodes[i].port << "\n";
                std::cout << "-->> erpc port: " << cluster.nodes[i].erpc_port << "\n";
            }
            std::cout << ">> range group: \n";
            for (size_t j = 0; j < group.num_infos; j++) {
                std::cout << "-->> range[" << j << "]: " << group.infos[j].start << "\n";
                std::cout << "-->> version: " << group.infos[j].version << "\n";
                std::cout << "-->> nodes: \n";
                for(size_t t = 0; t < Constants::uMAX_NODE; t++) {
                    if (group.infos[j].nodes[t] != 0) {
                        std::cout << "---->> node " << int(group.infos[j].nodes[t]) << "\n";
                        std::cout << "---->> is_mem: " << group.infos[j].is_mem[t] << "\n";
                    }
                }
            }
        }

        auto Node::prepare(const std::string &configure_file) -> bool {
            auto _content = Misc::file_as_string(configure_file);
            if (!_content.has_value()) {
                return false;
            }

            auto content = _content.value();

            node_id = ConfigReader::read_node_id(content).value();
            total_pm = ConfigReader::read_total_pm(content).value();
            available_pm = ConfigReader::read_available_pm(content).value();
            // impossible be invalid
            addr = IPV4Addr::make_ipv4_addr(ConfigReader::read_ip_addr(content).value()).value();
            port = ConfigReader::read_ip_port(content).value();
            erpc_port = ConfigReader::read_erpc_port(content).value();
            erpc_listen_port = ConfigReader::read_erpc_listen_port(content).value();
            rpc_uri = addr.to_string() + ":" + std::to_string(erpc_port);
            monitor_addr = IPV4Addr::make_ipv4_addr(ConfigReader::read_monitor_addr(content).value()).value();
            monitor_port = ConfigReader::read_monitor_port(content).value();

            return true;
        }

        auto Node::launch() -> bool {
            run = true;
            auto sock = Misc::socket_connect(false, monitor_port, monitor_addr.to_string().c_str());
#if defined(__HILL_DEBUG__) || defined(__HILL_INFO__)
            std::cout << ">> Node at " << addr.to_string() << ":" << port << " connecting to monitor at "
                      << monitor_addr.to_string() << ":" << monitor_port << "\n";
#endif
            if (sock == -1) {
#if defined(__HILL_DEBUG__) || defined(__HILL_INFO__)
                std::cout << ">> Failed to connect to monitor\n";
#endif
                return false;
            }

            std::thread background([&, sock]() {
#if defined (__HILL__DEBUG) || defined (__HILL_INFO__)
                std::cout << ">> Monitor connected\n";
#endif
                auto total = 0UL;
#ifdef __HILL_DEBUG__
                Misc::check_socket_read_write(Misc::recv_all(sock, &total, sizeof(total)));
                std::cout << ">> Receiving size of " << total << " from monitor\n";
#else
                Misc::recv_all(sock, &total, sizeof(total));
#endif
                auto buf = std::make_unique<byte_t[]>(total);
#ifdef __HILL_DEBUG__
                Misc::check_socket_read_write(Misc::recv_all(sock, buf.get(), total));
#else
                Misc::recv_all(sock, buf.get(), total);
#endif

                cluster_status.deserialize(buf.get());
#ifdef __HILL_DEBUG__
                std::cout << ">> Receiving following meta from monitor\n";
                cluster_status.dump();
#endif
                cluster_status.cluster.nodes[node_id].version = 1;
                cluster_status.cluster.nodes[node_id].node_id = node_id;
                cluster_status.cluster.nodes[node_id].total_pm = total_pm;
                cluster_status.cluster.nodes[node_id].addr = addr;
                cluster_status.cluster.nodes[node_id].port = port;
                cluster_status.cluster.nodes[node_id].erpc_port = erpc_port;
                cluster_status.cluster.nodes[node_id].erpc_listen_port = erpc_listen_port;
                cluster_status.cluster.nodes[node_id].is_active = true;

                while(run) {
                    keepalive(sock);
                }
                shutdown(sock, 0);
            });
            background.detach();
            return true;
        }

        auto Node::stop() -> void {
            run = false;
        }

        // I need extra infomation to update PM usage and CPU usage
        auto Node::keepalive(int socket) noexcept -> bool {
#ifdef __HILL_DEBUG__
            std::cout << "---->> this in keepalive " << this << "\n";
#endif
            auto to_size = cluster_status.total_size();
            // Atomicity is not the first concern, because all these data fields are concurrently atomic
            cluster_status.cluster.nodes[node_id].available_pm = available_pm;
            cluster_status.cluster.nodes[node_id].cpu_usage = cpu_usage;

            ++cluster_status.cluster.nodes[node_id].version;
            ++cluster_status.version;
            // all machines are little-endian
#ifdef __HILL_DEBUG__
            Misc::check_socket_read_write(Misc::send_all(socket, &to_size, sizeof(to_size)), false);
            std::cout << ">> Writing size of " << to_size << " to monitor\n";
#else
            Misc::send_all(socket, &to_size, sizeof(to_size));
#endif

            auto to_buf = cluster_status.serialize();
#ifdef __HILL_DEBUG__
            std::cout << ">> Writnig following meta to monitor\n";
            Misc::check_socket_read_write(Misc::send_all(socket, to_buf.get(), to_size), false);
            cluster_status.dump();
#else
            Misc::send_all(socket, to_buf.get(), to_size);
#endif

            auto size = 0UL;
#ifdef __HILL_DEBUG__
            Misc::recv_all(socket, &size, sizeof(size));
            std::cout << ">> Receiving size of " << size << " from monitor\n";
#else
            Misc::recv_all(socket, &size, sizeof(size));
#endif

            auto buf = std::make_unique<byte_t[]>(size);
#ifdef __HILL_DEBUG__
            Misc::check_socket_read_write(Misc::recv_all(socket, buf.get(), size));
#else
            Misc::recv_all(socket, buf.get(), size);
#endif
            ClusterMeta tmp;
            tmp.deserialize(buf.get());
#ifdef __HILL_DEBUG__
            std::cout << ">> Receiving following meta from monitor\n";
            cluster_status.dump();
#endif
            cluster_status.update(tmp);
#ifdef __HILL_DEBUG__
            cluster_status.dump();
            std::cout << "\n\n\n";
#endif
            sleep(3);
            return true;
        }

        auto Node::dump() const noexcept -> void {
            std::cout << ">> Node info: \n";
            std::cout << "-->> Node ID: " << node_id << "\n";
            std::cout << "-->> Total PM: " << total_pm << "\n";
            std::cout << "-->> Available PM: " << available_pm << "\n";
            std::cout << "-->> IP Addr: " << addr.to_string() << "\n";
            std::cout << "-->> Monitor Addr: " << monitor_addr.to_string() << "\n";
            std::cout << "-->> Monitor Port: " << monitor_port << "\n";
        }

        auto Monitor::prepare(const std::string &configure_file) -> bool {
            auto content_ = Misc::file_as_string(configure_file);
            if (!content_.has_value()) {
                return false;
            }

            auto content = content_.value();

            std::regex rnode_num("node_num:\\s*(\\d+)");
            std::regex rranges("range:\\s*((\\S+),\\s*(\\d+))");
            std::regex raddr("addr:\\s*(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d+)");

            std::smatch vnode_num, vranges, vaddr;
            if (!std::regex_search(content, vnode_num, rnode_num)) {
                std::cerr << ">> Error: invalid or unspecified node num\n";
                return false;
            }

            if (!std::regex_search(content, vaddr, raddr)) {
                std::cerr << ">> Error: invalid IP address\n";
                return false;
            }

            addr = IPV4Addr::make_ipv4_addr(vaddr[1].str()).value();
            port = atoi(vaddr[2].str().c_str());
            meta.cluster.node_num = atoi(vnode_num[1].str().c_str());

            while (std::regex_search(content, vranges, rranges)) {
                meta.group.add_main(vranges[2].str(), atoi(vranges[3].str().c_str()));
                content = vranges.suffix();
            }

            return true;
        }

        auto Monitor::launch() -> bool {
            run = true;
            auto sock = Misc::make_async_socket(true, port);

            if (sock == -1) {
                return false;
            }
#if defined(__HILL_DEBUG__) || defined(__HILL_INFO__)
            std::cout << ">> Monitor running at " << addr.to_string() << ":" << port << "\n";
#endif
            std::thread work([&, sock]() {
                while(run) {
                    check_income_connection(sock);
                    sleep(1);
                }
                shutdown(sock, 0);
            });
            work.detach();
            return true;
        }

        auto Monitor::stop() -> void {
            run = false;
        }

        auto Monitor::check_income_connection(int sock) -> void {
            // accept should be non-blocking, but read/write should be blocking
            auto socket = Misc::accept_blocking(sock);
            if (socket == -1) {
#ifdef __HILL_DEBUG__
                std::cout << ">> No new connection is detected\n";
#endif
                return;
            }
#ifdef __HILL_DEBUG__
            std::cout << ">> New node is connected\n";
            auto flags = fcntl(socket, F_GETFL);
            std::cout << ">> New socket is in non-blocking mode: " << (flags & O_NONBLOCK) << "\n";
#endif

            std::thread heartbeat([&, socket]() {
                // on first connection
                auto to_size = meta.total_size();
#ifdef __HILL_DEBUG__
                std::cout << ">> Sending size of " << to_size << " to server node\n";
                Misc::check_socket_read_write(Misc::send_all(socket, &to_size, sizeof(to_size)), false);
#else
                Misc::send_all(socket, &to_size, sizeof(to_size));
#endif

                auto to_buf = meta.serialize();
#ifdef __HILL_DEBUG__
                Misc::check_socket_read_write(Misc::send_all(socket, to_buf.get(), to_size), false);
                std::cout << ">> Sending following meta to server node:\n";
                meta.dump();
#else
                Misc::send_all(socket, to_buf.get(), to_size);
#endif

                // keepalive
                while(run) {
                    ClusterMeta tmp;
                    auto size = 0UL;
#ifdef __HILL_DEBUG__
                    Misc::check_socket_read_write(Misc::recv_all(socket, &size, sizeof(size)));
                    std::cout << ">> Receiving size of " << size << " from server node\n";
#else
                    Misc::recv_all(socket, &size, sizeof(size));
#endif
                    auto buf = std::make_unique<byte_t[]>(size);
#ifdef __HILL_DEBUG__
                    Misc::check_socket_read_write(Misc::recv_all(socket, buf.get(), size));
                    std::cout << ">> Receiving following meta from server node\n";
                    tmp.deserialize(buf.get());
                    tmp.dump();
#else
                    Misc::recv_all(socket, buf.get(), size);
                    tmp.deserialize(buf.get());
#endif

                    meta.update(tmp);
                    return_cluster_meta(socket);
#ifdef __HILL_DEBUG__
                    std::cout << "\n\n\n";
#endif

                    sleep(1);
                }
            });
            heartbeat.detach();
        }

        auto Monitor::return_cluster_meta(int socket) noexcept -> void {
            ++meta.version;
            auto size= meta.total_size();
#ifdef __HILL_DEBUG__
            std::cout << ">> Sending size of " << size << " to server node\n";
            Misc::check_socket_read_write(Misc::send_all(socket, &size, sizeof(size)), false);
#else
            Misc::send_all(socket, &size, sizeof(size));
#endif
            auto buf = meta.serialize();
#ifdef __HILL_DEBUG__
            std::cout << ">> Sending following meta to server node:\n";
            meta.dump();
            Misc::check_socket_read_write(Misc::send_all(socket, buf.get(), size), false);
#else
            Misc::send_all(socket, buf.get(), size);
#endif
        }

        auto Monitor::dump() const noexcept -> void {
            std::cout << ">> Monitor info: \n";
            std::cout << "-->> Addr: " << addr.to_string() << ":" << port << "\n";
            std::cout << "-->> Meta:\n";
            meta.dump();
        }
    }
}
