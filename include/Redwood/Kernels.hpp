#pragma once

#include "Point.hpp"

namespace redwood {

// Kernel Related
// Bh
void ComputeOneBatchAsync(const int* u_leaf_indices, /**/
                          int num_active_leafs,      /**/
                          float* out,                /**/
                          const Point4F* u_lnt_data, /**/
                          const int* u_lnt_sizes,    /**/
                          Point4F q,                 /**/
                          int stream_id);

void ComputeOneBatchAsync_PB(const int* u_leaf_indices,  /**/
                          const int num_active_leafs, /**/
                          float* out,                 /**/
                          const Point4F* u_lnt_data,  /**/
                          const int* u_lnt_sizes,     /**/
                          const Point4F q,            /**/
                          const int stream_id,
                          const int pb_idx);
// Knn
void ProcessKnnAsync(const int* u_leaf_indices, /**/
                     const Point4F* u_q_points, /**/
                     int num_active_leafs,      /**/
                     float* out,                /* stream base addr */
                     const Point4F* u_lnt_data, /**/
                     const int* u_lnt_sizes,    /**/
                     int stream_id);

}  // namespace redwood