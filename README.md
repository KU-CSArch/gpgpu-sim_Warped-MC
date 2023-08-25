## Warped-MC
This repository contains the [gpgpu-sim v4.2](https://github.com/accel-sim/gpgpu-sim_distribution) implementation of Warped-MC, an efficient memory controller scheme for massively parallel processors.
Warped-MC is published on International Conference on Parallel Processing (**ICPP**) ,August 2023.

### Simulation on Warped-MC

 1. edit option to gpgpusim.config

`-gpgpu_frfcfs_option`

 2. select memory controller scheme
 - -gpgpu_dram_scheduler 0 = fifo, 1 = FR-FCFS (defaul), 2 = divergence_first
 
 Note that it needs '-gpgpu_dram_scheduler 1' option 
 - -gpgpu_frfcfs_option 0 = FR-FCFS (default), 1 = Warped-MC  
 


### Author
**Jong Hyun Jeong**
 - e-mail : dida1245@korea.ac.kr
 
