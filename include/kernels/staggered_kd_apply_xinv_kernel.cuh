#include <color_spinor_field_order.h>
#include <gauge_field_order.h>
#include <multigrid_helper.cuh>
#include <index_helper.cuh>
#include <register_traits.h>
#include <cub_helper.cuh>

namespace quda {

  template <typename vFloatSpinor_, typename vFloatGauge_, int coarseDof_, int fineColor_,
            bool dagger_, typename fineColorSpinor, typename xInvGauge>
  struct ApplyStaggeredKDBlockArg {

    using vFloatSpinor = vFloatSpinor_;
    using vFloatGauge = vFloatGauge_;

    using Float = typename mapper<vFloatGauge>::type;

    static constexpr int fineColor = fineColor_;
    static constexpr int fineSpin = 1;
    static constexpr int coarseDof = coarseDof_;
    static constexpr bool dagger = dagger_;

    static constexpr int blockSizeKD = 16; /** Elements per KD block */
    static constexpr int paddedSpinorSizeKD = blockSizeKD; // padding is currently broken + 1; /** Padded size */

    static constexpr int xinvRowTileSize = 16; /** Length of a tile for KD */
    static constexpr int xinvColTileSize = 16; /** Length of a tile for KD */
    static constexpr int xinvPaddedColTileSize = xinvColTileSize + 1; /** Padding to avoid bank conflicts */
    static constexpr int numTiles = (coarseDof + xinvColTileSize - 1) / xinvColTileSize; /** Number of tiles, should always be three. */

    fineColorSpinor out;      /** Output staggered spinor field */
    const fineColorSpinor in; /** Input staggered spinor field */
    const xInvGauge xInv;     /** Kahler-Dirac inverse field */

    int_fastdiv x_size[QUDA_MAX_DIM];           /** Dimensions of fine grid */
    int_fastdiv xc_size[QUDA_MAX_DIM];  /** Dimensions of coarse grid */

    const int fineVolumeCB;
    const int_fastdiv coarseVolumeCB;   /** Coarse grid volume */

    ApplyStaggeredKDBlockArg(fineColorSpinor &out, const fineColorSpinor &in, const xInvGauge &xInv,
                           const int *x_size_, const int *xc_size_) :
      out(out),
      in(in),
      xInv(xInv),
      fineVolumeCB(in.volumeCB),
      coarseVolumeCB(xInv.VolumeCB())
    {
      for (int i=0; i<QUDA_MAX_DIM; i++) {
        x_size[i] = x_size_[i];
        xc_size[i] = xc_size_[i];
      }
    }

  };

  template<typename Arg>
  __global__ void ApplyStaggeredKDBlockGPU(Arg arg)
  {

    // Vector type for spinor
    using real_spinor = typename mapper<typename Arg::vFloatSpinor>::type;
    using complex_spinor = complex<real_spinor>;
    using Vector = ColorSpinor<real_spinor, Arg::fineColor, 1>;

    // Type for gauge/compute
    using real = typename Arg::Float;
    using complex = complex<real>;
    extern __shared__ complex cs_buffer[];

    // For each "dot product" in the mat-vec
    typedef cub::WarpReduce<real,16> WarpReduce16;
    __shared__ typename WarpReduce16::TempStorage temp_storage_16;

    /////////////////////////////////
    // Figure out some identifiers //
    /////////////////////////////////

    // What is my overall thread id?
    const unsigned int tid = blockIdx.x * blockDim.x + threadIdx.x; //((blockIdx.y*gridDim.x + blockIdx.x)*blockDim.y + threadIdx.y)*blockDim.x + threadIdx.x;

    if (tid >= arg.fineVolumeCB*2) return;

    // Decompose into factors of 16.
    const unsigned int fast_idx = tid & 0b1111;
    const unsigned int mid_idx = (tid >> 4) & 0b1111;
    const unsigned int slow_idx = tid >> 8;

    // The fundamental unit of work is 256 threads = 16 KD blocks
    // What's the first KD block in my unit of work?
    const unsigned int x_coarse_first = 16 * slow_idx;

    unsigned int x_coarse_tmp, coarse_x, coarse_y, coarse_z, coarse_t;

    // FIXME: switch to using getParityCBFromFull in index_helper.cuh.
    
    // What's my KD block for loading Xinv?
    // [0-15] loads consecutive elements of Xinv for the first block,
    // [16-31] loads consecutive elements of Xinv for the second block, etc
    const unsigned int x_coarse_xinv = x_coarse_first + mid_idx;
    const unsigned int x_coarse_xinv_cb = x_coarse_xinv >> 1;

    x_coarse_tmp = x_coarse_xinv;
    coarse_x = x_coarse_tmp % arg.xc_size[0];
    x_coarse_tmp = x_coarse_tmp / arg.xc_size[0];
    coarse_y = x_coarse_tmp % arg.xc_size[1];
    x_coarse_tmp = x_coarse_tmp / arg.xc_size[1];
    coarse_z = x_coarse_tmp % arg.xc_size[2];
    coarse_t = x_coarse_tmp / arg.xc_size[2];

    const unsigned int parity_coarse_xinv = (coarse_x + coarse_y + coarse_z + coarse_t) & 1;

    // What's my KD block for loading spinors?
    // [0-15] loads first corner of consecutive hypercubes,
    // [16-31] loads the second corner, etc
    const unsigned int x_coarse_spinor = x_coarse_first + fast_idx;
    const unsigned int x_coarse_spinor_cb = x_coarse_spinor >> 1;
    
    // There's got to be a better way to do this
      x_coarse_tmp = x_coarse_spinor;
      coarse_x = x_coarse_tmp % arg.xc_size[0];
      x_coarse_tmp = x_coarse_tmp / arg.xc_size[0];
      coarse_y = x_coarse_tmp % arg.xc_size[1];
      x_coarse_tmp = x_coarse_tmp / arg.xc_size[1];
      coarse_z = x_coarse_tmp % arg.xc_size[2];
      coarse_t = x_coarse_tmp / arg.xc_size[2];

    const unsigned int parity_coarse_spinor = (coarse_x + coarse_y + coarse_z + coarse_t) & 1;

    //printf("coarseIdx %d %d %d %d %d %d %d\n", x_coarse_first,
    //        x_coarse_spinor, x_coarse_spinor_cb, parity_coarse_spinor, 
    //        x_coarse_xinv, x_coarse_xinv_cb, parity_coarse_xinv);

    /////////////////////////////////////
    // Set up my shared memory buffers //
    /////////////////////////////////////

    // This is complicated b/c we need extra padding to avoid bank conflcits
    // Check `staggered_kd_apply_xinv.cu` for the dirty details
    constexpr int buffer_size = Arg::fineColor * 16 * Arg::paddedSpinorSizeKD; //256 * ((Arg::fineColor * 16 * Arg::paddedSpinorSizeKD * sizeof(complex)) / 256 + 1);

    // size of tile: 
    constexpr int fullTileSize = 16 * Arg::xinvRowTileSize * Arg::xinvPaddedColTileSize;

    // Which unit of spinor work am I within this block?
    const int unit_of_work = threadIdx.x / 256; // (threadIdx.x + threadIdx.y * blockDim.x) / 256;
    complex* in_buffer = cs_buffer + (2 * unit_of_work) * buffer_size;
    complex* out_buffer = cs_buffer + (2 * unit_of_work + 1) * buffer_size;

    // Which unit of Xinv tile am I?
    complex* xinv_buffer = cs_buffer + 2 * (blockDim.x / 256) * buffer_size + unit_of_work * fullTileSize + mid_idx * Arg::xinvRowTileSize * Arg::xinvPaddedColTileSize;
    
    ////////////////////////////////////////////////////
    // Hey, real work! What ColorVector am I loading? //
    ////////////////////////////////////////////////////
    
    int coarseCoords[4];
    getCoords(coarseCoords, x_coarse_spinor_cb, arg.xc_size, parity_coarse_spinor);

    // What corner of the hypercube am I grabbing?
    int tmp_mid_idx = mid_idx;
    int y_bit = tmp_mid_idx & 1; tmp_mid_idx >>= 1;
    int z_bit = tmp_mid_idx & 1; tmp_mid_idx >>= 1;
    int t_bit = tmp_mid_idx & 1; tmp_mid_idx >>= 1;
    int spin_bit = tmp_mid_idx;

    int x_bit = (y_bit + z_bit + t_bit) & 1;
    if (spin_bit) x_bit = 1 - x_bit;

    int parity_spinor = (x_bit + y_bit + z_bit + t_bit) & 1;

    // Last xc_size[0] is intentional
    int x_spinor_cb = (((((2 * coarseCoords[3] + t_bit) * arg.x_size[2]) + 2 * coarseCoords[2] + z_bit) * arg.x_size[1] + 2 * coarseCoords[1] + y_bit) * arg.x_size[0] + 2 * coarseCoords[0] + x_bit) >> 1;

    //  Load into:     (0th fine color)  + (coarse spin) + (Xinv offset)
    int buffer_index = (mid_idx & 0b111) + 24 * spin_bit + Arg::fineColor * Arg::paddedSpinorSizeKD * fast_idx;

    const Vector in = arg.in(x_spinor_cb, parity_spinor);

    // ♫ do you believe in bank conflicts ♫
    for (int c_f = 0; c_f < Arg::fineColor; c_f++) {
      in_buffer[buffer_index + 8 * c_f] = static_cast<complex>(in(0,c_f)); // { (real)(in_spinor.real()), (real)(in_spinor.imag()) };
    }

    // in reality we only need to sync over my chunk of 256 threads
    __syncthreads();

    /////////////////////////////
    // Multiply by Xinv, store //
    /////////////////////////////

    // Zero the shared memory buffer, which is 48 components
    #pragma unroll
    for (int coarse_row = fast_idx; coarse_row < Arg::coarseDof; coarse_row += Arg::blockSizeKD) {
      out_buffer[Arg::paddedSpinorSizeKD * Arg::fineColor * mid_idx + coarse_row] = { 0, 0 };
    }

    #pragma unroll
    for (int tile_row = 0; tile_row < Arg::numTiles; tile_row++) {
      #pragma unroll
      for (int tile_col = 0; tile_col < Arg::numTiles; tile_col++) {
        // load Xinv
        if (Arg::dagger) {
          #pragma unroll
          for (int col = 0; col < Arg::xinvColTileSize; col++) {  
            xinv_buffer[fast_idx * Arg::xinvPaddedColTileSize + col] = conj(arg.xInv(0, parity_coarse_xinv, x_coarse_xinv_cb, 0, 0, Arg::xinvColTileSize * tile_col + col, Arg::xinvRowTileSize * tile_row + fast_idx));
          }
        } else {
          #pragma unroll
          for (int row = 0; row < Arg::xinvColTileSize; row++) {  
            xinv_buffer[row * Arg::xinvPaddedColTileSize + fast_idx] = arg.xInv(0, parity_coarse_xinv, x_coarse_xinv_cb, 0, 0, Arg::xinvRowTileSize * tile_row + row, Arg::xinvColTileSize * tile_col + fast_idx);
          }
        }

        __syncwarp();

        // do the tile multiplication
        #pragma unroll
        for (int row = 0; row < Arg::xinvRowTileSize; row++) {
          const complex xinv_elem = xinv_buffer[row * Arg::xinvPaddedColTileSize + fast_idx];
          const complex cs_component = in_buffer[Arg::coarseDof * mid_idx + Arg::xinvColTileSize * tile_col + fast_idx];
          const complex prod = cmul(xinv_elem, cs_component);

          const real re_sum = WarpReduce16(temp_storage_16).Sum(prod.real());
          const real im_sum = WarpReduce16(temp_storage_16).Sum(prod.imag());

          __syncwarp();

          if (fast_idx == 0)
          {
            out_buffer[Arg::paddedSpinorSizeKD * Arg::fineColor * mid_idx + Arg::xinvRowTileSize * tile_row + row].x += re_sum; 
            out_buffer[Arg::paddedSpinorSizeKD * Arg::fineColor * mid_idx + Arg::xinvRowTileSize * tile_row + row].y += im_sum; 
          }
        }
      }
    }


    __syncthreads();

    /////////////////////////////////////
    // Store: one whole thing is easy! //
    /////////////////////////////////////

    Vector out;

    #pragma unroll
    for (int c_f = 0; c_f < Arg::fineColor; c_f++) {
      out(0,c_f) = static_cast<complex_spinor>(out_buffer[buffer_index + 8 * c_f]);
    }

    arg.out(x_spinor_cb, parity_spinor) = out;

  }

} // namespace quda
