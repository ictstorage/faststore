#include "cluster.hpp"
#include "misc/misc.hpp"

#include <iostream>
#include <fstream>

namespace Hill {
    namespace Cluster {
        auto RangeGroup::add_main(const std::string &s, int node_id) noexcept -> void {
            if (node_id == 0) {
                std::cerr << ">> Error: node 0 is not supposed to be in range group\n";
                return;
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

        auto RangeGroup::append_node(const std::string &s, int node_id, bool is_mem) noexcept -> void {
            if (node_id == 0) {
                std::cerr << ">> Error: node 0 is not supposed to be in range group\n";
                return;
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
            auto total_size = sizeof(cluster);
            // num_infos
            total_size += sizeof(group.num_infos);
            // dynamic field
            for (size_t i = 0; i < group.num_infos; i++) {
                total_size += sizeof(uint64_t);
                total_size += group.infos[i].start.size();
                total_size += sizeof(group.infos[i].is_mem);
                total_size += sizeof(group.infos[i].nodes);
            }
            return total_size;
        }

        auto ClusterMeta::serialize() const noexcept -> std::unique_ptr<uint8_t[]> {
            auto buf = new uint8_t[total_size()];
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

            return std::unique_ptr<uint8_t[]>(buf);
        }

        auto ClusterMeta::deserialize(const uint8_t *buf) -> void {
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

        auto ClusterMeta::dump() const noexcept -> void {
            std::cout << "--------------------- Meta Info --------------------- \n";
            std::cout << ">> version: " << version << "\n";
            std::cout << ">> node num: " << cluster.node_num << "\n";
            std::cout << ">> node info: \n";
            for (size_t i = 0; i < cluster.node_num; i++) {
                std::cout << ">> node " << i << "\n";
                std::cout << "-->> version: " << cluster.nodes[i].version << "\n";
                std::cout << "-->> node id: " << cluster.nodes[i].node_id << "\n";
                std::cout << "-->> total pm: " << cluster.nodes[i].total_pm << "\n";
                std::cout << "-->> availabel pm: " << cluster.nodes[i].available_pm << "\n";
                std::cout << "-->> ip address: " << cluster.nodes[i].addr.to_string() << "\n";
            }
            std::cout << ">> range group: \n";
            for (size_t j = 0; j < group.num_infos; j++) {
                std::cout << "-->> range[" << j << "]: " << group.infos[j].start << "\n";
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
            std::ifstream configuration(configure_file);
            if (!configuration.is_open()) {
                return false;
            }

            std::stringstream buf;
            buf << configuration.rdbuf();
            auto content = buf.str();

            std::regex rnode_id("node_id:\\s*(\\d+)");
            std::regex rtotal_pm("total_pm:\\s*(\\d+)");
            std::regex ravailable_pm("available_pm:\\s*(\\d+)");            
            std::regex raddr("addr:\\s*(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3})");
            std::regex rmonitor("monitor:\\s*(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}):(\\d+)");

            std::smatch vnode_id, vtotal_pm, vaddr, vavailable_pm, vmonitor;
            if (!std::regex_search(content, vnode_id, rnode_id)) {
                std::cerr << ">> Error: invalid or unspecified node id\n";
                return false;
            }

            if (!std::regex_search(content, vtotal_pm, rtotal_pm)) {
                std::cerr << ">> Error: invalid or unspecified total PM\n";
                return false;
            }

            if (!std::regex_search(content, vavailable_pm, ravailable_pm)) {
                std::cerr << ">> Error: invalid or unspecified available PM\n";
                return false;
            }
            
            if (!std::regex_search(content, vaddr, raddr)) {
                std::cerr << ">> Error: invalid or unspecified IP address\n";
                return false;
            }

            if (!std::regex_search(content, vmonitor, rmonitor)) {
                std::cerr << ">> Error: invalid or unspecified monitor\n";
                return false;
            }            

            node_id = atoi(vnode_id[1].str().c_str());
            total_pm = atoll(vtotal_pm[1].str().c_str());
            available_pm = atoll(vavailable_pm[1].str().c_str());
            // impossible be invalid
            addr = IPV4Addr::make_ipv4_addr(vaddr[1].str()).value();
            monitor_addr = IPV4Addr::make_ipv4_addr(vmonitor[1].str()).value();
            monitor_port = atoi(vmonitor[2].str().c_str());
            return true;
        }

        auto Node::launch() -> void {
            run = true;
            std::thread background([&]() {
                while(run) {
                    keepalive();
                }
            });
            background.detach();
        }

        auto Node::stop() -> void {
            run = false;
        }

        // I need extra infomation to update PM usage and CPU usage
        auto Node::keepalive() const noexcept -> bool {
            auto sock = Misc::socket_connect(false, monitor_port, monitor_addr.to_string().c_str());
            if (sock == -1) {
                return false;
            }

            // TODO
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
    }
}
