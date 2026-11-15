#pragma once
#include "BulletSoftBody/btSoftBody.h"
#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace mikumikustudio::pmx21::topology {

inline void AppendFaces(btSoftBody& body, const std::vector<int>& faces,
                        btSoftBody::Material* material) {
    std::unordered_set<std::uint64_t> edges;
    edges.reserve(faces.size());
    const auto link = [&](int a, int b) {
        const auto key = (std::uint64_t(std::min(a,b)) << 32) | std::uint32_t(std::max(a,b));
        // Preserve the first face's edge direction and insertion order, without
        // Bullet's linear search through every previously inserted link.
        if (edges.insert(key).second) body.appendLink(a,b,material);
    };
    for (std::size_t i=0;i+2<faces.size();i+=3) {
        const int a=faces[i], b=faces[i+1], c=faces[i+2];
        if (a==b || b==c || a==c) continue;
        body.appendFace(a,b,c,material);
        link(a,b); link(b,c); link(c,a);
    }
}

inline int GenerateBending(btSoftBody& body, int distance, btSoftBody::Material* material) {
    const int count=body.m_nodes.size();
    if (distance<=1 || distance>=count) return 0;
    std::vector<std::vector<int>> neighbors(count);
    for (int i=0;i<body.m_links.size();++i) {
        const auto& link=body.m_links[i];
        const int a=int(link.m_n[0]-&body.m_nodes[0]), b=int(link.m_n[1]-&body.m_nodes[0]);
        neighbors[a].push_back(b); neighbors[b].push_back(a);
    }
    // The original builds a dense all-pairs distance matrix. Only the ring at
    // the requested graph distance is needed; stamp visits to avoid clearing
    // a full node array for every source. Added bending links never enter BFS.
    std::vector<int> visited(count,-1), queue;
    int added=0;
    for (int source=0;source<count;++source) {
        queue.clear(); queue.push_back(source); visited[source]=source;
        std::size_t first=0, last=1;
        for (int depth=0;depth<distance && first<last;++depth) {
            for (std::size_t i=first;i<last;++i) {
                for (int next:neighbors[queue[i]]) if (visited[next]!=source) {
                    visited[next]=source; queue.push_back(next);
                }
            }
            first=last; last=queue.size();
        }
        // Bullet appends (higher,lower) in lexicographic (lower,higher) order.
        // Keeping that order also preserves subsequent randomization and CPU
        // Gauss-Seidel behavior.
        std::sort(queue.begin()+first,queue.end());
        for (std::size_t i=first;i<last;++i) if (queue[i]>source) {
            body.appendLink(queue[i],source,material);
            body.m_links[body.m_links.size()-1].m_bbending=1;
            ++added;
        }
    }
    return added;
}
}
