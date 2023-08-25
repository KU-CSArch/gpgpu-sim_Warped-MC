// Copyright (c) 2009-2011, Tor M. Aamodt, Ali Bakhoda, George L. Yuan,
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution. Neither the name of
// The University of British Columbia nor the names of its contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "dram_sched.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

// JH
divergence_first_scheduler::divergence_first_scheduler(const memory_config *config) {
  m_config = config;
  m_queue = new std::list<dram_req_t *>[m_config->nbk];
}

frfcfs_scheduler::frfcfs_scheduler(const memory_config *config, dram_t *dm,
                                   memory_stats_t *stats) {
  m_config = config;
  m_stats = stats;
  m_num_pending = 0;
  m_num_write_pending = 0;
  m_dram = dm;
  m_queue = new std::list<dram_req_t *>[m_config->nbk];
  m_bins = new std::map<
      unsigned, std::list<std::list<dram_req_t *>::iterator> >[m_config->nbk];
  m_last_row =
      new std::list<std::list<dram_req_t *>::iterator> *[m_config->nbk];
  curr_row_service_time = new unsigned[m_config->nbk];
  row_service_timestamp = new unsigned[m_config->nbk];
  for (unsigned i = 0; i < m_config->nbk; i++) {
    m_queue[i].clear();
    m_bins[i].clear();
    m_last_row[i] = NULL;
    curr_row_service_time[i] = 0;
    row_service_timestamp[i] = 0;
  }
  // JH
  m_row_scoreboard.resize(m_config->nbk);
  for(unsigned i=0; i<m_config->nbk; i++) {
    m_row_scoreboard[i].clear();
  }
  if (m_config->seperate_write_queue_enabled) {
    m_write_queue = new std::list<dram_req_t *>[m_config->nbk];
    m_write_bins = new std::map<
        unsigned, std::list<std::list<dram_req_t *>::iterator> >[m_config->nbk];
    m_last_write_row =
        new std::list<std::list<dram_req_t *>::iterator> *[m_config->nbk];

    for (unsigned i = 0; i < m_config->nbk; i++) {
      m_write_queue[i].clear();
      m_write_bins[i].clear();
      m_last_write_row[i] = NULL;
    }
  }
  m_mode = READ_MODE;
}

void frfcfs_scheduler::add_req(dram_req_t *req) {
  if (m_config->seperate_write_queue_enabled && req->data->is_write()) {
    assert(m_num_write_pending < m_config->gpgpu_frfcfs_dram_write_queue_size);
    m_num_write_pending++;
    // JH : request input cycle
    req->age_cycle = req->m_gpu->gpu_sim_cycle + req->m_gpu->gpu_tot_sim_cycle; // write

    m_write_queue[req->bk].push_front(req);
    std::list<dram_req_t *>::iterator ptr = m_write_queue[req->bk].begin();
    m_write_bins[req->bk][req->row].push_front(ptr);  // newest reqs to the
                                                      // front
  } else {
    assert(m_num_pending < m_config->gpgpu_frfcfs_dram_sched_queue_size);
    m_num_pending++;
    if (m_config->scheduler_type == DRAM_FRFCFS){
      bool check_new = false;
      if (m_config->frfcfs_option == DEFAULT || m_config->frfcfs_option == WARPED_MC  ) { // JH : row priority
        if (m_row_scoreboard[req->bk].find(req->row) == m_row_scoreboard[req->bk].end() ) { // JH : no row entry
          m_row_scoreboard[req->bk].insert(std::pair<unsigned, row_stat_t>(req->row, row_stat_t()) ); // new row entry
          check_new = true;
        }
        m_row_scoreboard[req->bk][req->row].subscore++;
        
        address_type pc = req->data->get_pc();
        unsigned sid = req->data->get_sid();
        unsigned wid = req->data->get_wid();
        if ( m_dram->m_gpu->m_warp_collector[pc][sid][wid].pendings == 1 ) { // first request in a warp or single request warp
          assert(!req->add_delay);
          assert(m_dram->m_gpu->m_warp_collector[pc][sid][wid].check_delay == false);
          req->score += req->pending_cycle;
          req->add_delay = true;
          m_dram->m_gpu->m_warp_collector[pc][sid][wid].check_delay = true;
          m_row_scoreboard[req->bk][req->row].score++;
          req->l2_prio = true; 
          req->prio = 2; // the last request
        } else if (m_dram->m_gpu->m_warp_collector[pc][sid][wid].pendings > 1 
        && m_dram->m_gpu->m_warp_collector[pc][sid][wid].check_delay == true
        && m_dram->m_gpu->m_warp_collector[pc][sid][wid].complete == false
        ) { 
          
          std::list<dram_req_t *>::iterator it;
          unsigned is_delay = 0;
          unsigned wrong_row = 0;
          for (unsigned i=0; i<m_config->nbk; i++) {
            for (it=m_queue[req->bk].begin(); it != m_queue[req->bk].end(); it++ ) {
              mem_fetch *mf = (*it)->data;
              if (mf->get_pc() == pc && mf->get_sid() == sid && mf->get_wid() == wid) {
                if( (*it)->add_delay == true ) { // This request is not the last reqeust of a warp
                  (*it)->score -= (*it)->pending_cycle; 
                  assert((*it)->score >= 0);
                  (*it)->add_delay = false;
                  wrong_row = (*it)->row;
                  is_delay++;
                  (*it)->l2_prio = false;
                  (*it)->l2_score = 0;
                  (*it)->prio = 0;                 
                }
              }
            }
          }
          if (is_delay && !check_new) { // adjust row scoreboard
            //if (is_delay > 1) printf("JH_debug, is_delay error %d\n", is_delay);
            if (m_row_scoreboard[req->bk][wrong_row].score > 0) {
              //m_row_scoreboard[req->bk][wrong_row].score--;
              m_row_scoreboard[req->bk][wrong_row].score -= is_delay;
              assert(m_row_scoreboard[req->bk][wrong_row].score >= 0);
              m_dram->m_gpu->m_warp_collector[pc][sid][wid].check_delay = false;
            } else {
              //printf("JH_debug, wrong row score %d\n", m_row_scoreboard[req->bk][wrong_row].score);
              assert(0);
            }
          }        
        }
      }
    }
    m_queue[req->bk].push_front(req);
    std::list<dram_req_t *>::iterator ptr = m_queue[req->bk].begin();
    m_bins[req->bk][req->row].push_front(ptr);  // newest req to the front
    req->age_cycle = req->m_gpu->gpu_sim_cycle + req->m_gpu->gpu_tot_sim_cycle; // read
  }
}
// JH

void frfcfs_scheduler::update_row_scoreboard(address_type pc, unsigned sid, unsigned wid) {
  for (unsigned b=0; b<m_config->nbk; b++) { // bank
    std::list<dram_req_t *>::iterator it;
    for (it = m_queue[b].begin(); it != m_queue[b].end(); it++) { // check all pending requests
      mem_fetch *mf = (*it)->data;
      if (mf->get_pc() == pc && mf->get_sid() == sid && mf->get_wid() == wid) {
        unsigned prio_row = (*it)->row;
        m_row_scoreboard[b][prio_row].score += 1;
      }
    }
  }
}
// JH

void frfcfs_scheduler::update_request_score(address_type pc, unsigned sid, unsigned wid) {
  for (unsigned b=0; b<m_config->nbk; b++) { // bank
    std::list<dram_req_t *>::iterator it;
    for (it = m_queue[b].begin(); it != m_queue[b].end(); it++) { // all pending requests
      mem_fetch *mf = (*it)->data;
      if (mf->get_pc() == pc && mf->get_sid() == sid && mf->get_wid() == wid) {
        (*it)->score += 4;
      } 
    }
  }
}
// JH

void frfcfs_scheduler::update_last_request_score(address_type pc, unsigned sid, unsigned wid) {
  for (unsigned b=0; b<m_config->nbk; b++) {
    std::list<dram_req_t *>::iterator it;
    for (it = m_queue[b].begin(); it != m_queue[b].end(); it++) { // all pending requests
      mem_fetch *mf = (*it)->data;
      unsigned row = (*it)->row;
      unsigned pending_cycle = (*it)->pending_cycle;
      if (mf->get_pc() == pc && mf->get_sid() == sid && mf->get_wid() == wid) { // the last request of a warp
        //assert((*it)->add_delay == false);
        if ((*it)->add_delay == false ) {
          (*it)->score += pending_cycle;
          (*it)->add_delay = true;
          (*it)->l2_prio = true;
          (*it)->prio = 2;
        }
      } 
    }
  }
}

void frfcfs_scheduler::add_pending_req() {
  while (!m_dram->mrqq->empty())
  {
    dram_req_t *req = m_dram->mrqq->pop();
    // Power stats
    m_dram->m_stats->total_n_access++;

    if (req->data->get_type() == WRITE_REQUEST) {
      m_dram->m_stats->total_n_writes++;
    } else if (req->data->get_type() == READ_REQUEST) {
      m_dram->m_stats->total_n_reads++;
    }

    req->data->set_status(IN_PARTITION_MC_INPUT_QUEUE,
                          m_dram->m_gpu->gpu_sim_cycle + m_dram->m_gpu->gpu_tot_sim_cycle);
    add_req(req);
  }
}

void frfcfs_scheduler::update_request_age_cycle() {
  for (unsigned i=0; i<m_config->nbk; i++) {
    std::list<dram_req_t *>::iterator it;
    for (it = m_queue[i].begin(); it != m_queue[i].end(); it++) { // all pending requests
      (*it)->score += 1;
      if ((*it)->l2_prio) // add age to urgent request, can service the oldest urgent request first
        (*it)->l2_score +=1;
    }
  }
}

unsigned frfcfs_scheduler::check_delay_cycle(address_type pc, unsigned sid, unsigned wid) {
  unsigned num_req = 0;
  for (unsigned i=0; i<m_config->nbk; i++) {
    std::list<dram_req_t *>::iterator it;
    mem_fetch *mf;
    for (it = m_queue[i].begin(); it != m_queue[i].end(); it++) { // all pending request
      mf = (*it)->data;
      if (mf->get_pc() == pc && mf->get_sid() == sid && mf->get_wid() == wid) {
        num_req++;
        if ( (*it)->add_delay == true ) {
          (*it)->score -= (*it)->pending_cycle;
          (*it)->add_delay = false;
          assert((*it)->score >= 0);
          (*it)->l2_prio = false;
        }
      }
    }
  }
  return num_req;
}
void frfcfs_scheduler::data_collection(unsigned int bank) {
  if (m_dram->m_gpu->gpu_sim_cycle > row_service_timestamp[bank]) {
    curr_row_service_time[bank] =
        m_dram->m_gpu->gpu_sim_cycle - row_service_timestamp[bank];
    if (curr_row_service_time[bank] >
        m_stats->max_servicetime2samerow[m_dram->id][bank])
      m_stats->max_servicetime2samerow[m_dram->id][bank] =
          curr_row_service_time[bank];
  }
  curr_row_service_time[bank] = 0;
  row_service_timestamp[bank] = m_dram->m_gpu->gpu_sim_cycle;
  if (m_stats->concurrent_row_access[m_dram->id][bank] >
      m_stats->max_conc_access2samerow[m_dram->id][bank]) {
    m_stats->max_conc_access2samerow[m_dram->id][bank] =
        m_stats->concurrent_row_access[m_dram->id][bank];
  }
  m_stats->concurrent_row_access[m_dram->id][bank] = 0;
  m_stats->num_activates[m_dram->id][bank]++;
}

dram_req_t *frfcfs_scheduler::schedule(unsigned bank, unsigned curr_row) {
  // row
  bool rowhit = true;
  std::list<dram_req_t *> *m_current_queue = m_queue;
  std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >
      *m_current_bins = m_bins;
  std::list<std::list<dram_req_t *>::iterator> **m_current_last_row =
      m_last_row;

  if (m_config->seperate_write_queue_enabled) {
    if (m_mode == READ_MODE &&
        ((m_num_write_pending >= m_config->write_high_watermark)
         // || (m_queue[bank].empty() && !m_write_queue[bank].empty())
         )) {
      m_mode = WRITE_MODE;
    } else if (m_mode == WRITE_MODE &&
               ((m_num_write_pending < m_config->write_low_watermark)
                //  || (!m_queue[bank].empty() && m_write_queue[bank].empty())
                )) {
      m_mode = READ_MODE;
    }
  }

  if (m_mode == WRITE_MODE) {
    m_current_queue = m_write_queue;
    m_current_bins = m_write_bins;
    m_current_last_row = m_last_write_row;
  }

  if (m_current_last_row[bank] == NULL) { // JH : there are no active row
    if (m_current_queue[bank].empty()) return NULL; // JH : no pending requests

    std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >::iterator
        bin_ptr = m_current_bins[bank].find(curr_row);
    	// JH : row that issued the most recently
    if (bin_ptr == m_current_bins[bank].end()) { // JH : pending request in issued row doesn't exist
      m_row_scoreboard[bank].erase(curr_row); // delete current row entry

      std::list<dram_req_t *>::iterator max_score_req = m_current_queue[bank].begin();
      std::list<dram_req_t *>::iterator ip;
      for (ip=m_current_queue[bank].begin(); ip!=m_current_queue[bank].end();ip++) {
        if ((*max_score_req)->score < (*ip)->score ) max_score_req = ip;
      }
      unsigned max_score = 0;
      int max_row = -1;
      unsigned long long max_subscore = 0;
      std::map<unsigned, row_stat_t>::iterator it;
      unsigned n_row = 0;
      for (it = m_row_scoreboard[bank].begin(); it != m_row_scoreboard[bank].end(); it++) {   
        n_row++;
        // row priority: 1st - num of warp
        if (it->second.score > max_score ) {
          max_score = it->second.score;
          max_row = it->first;
          max_subscore = it->second.subscore;
        } else if ( it->second.score == max_score ) {
          if (max_row < 0) { // max_score == 0
            max_score = it->second.score;
            max_row = it->first;
            max_subscore = it->second.subscore;
          }
          //  // wa 3.0
          // if ( it->second.subscore > max_subscore ) {
          //   max_score = it->second.score;
          //   max_row = it->first;
          //   max_subscore = it->second.subscore;
          // }
        } 
      }

      if (m_config->frfcfs_option == WARPED_MC ) {  // row selection
        if (max_row >= 0) {
          bin_ptr = m_current_bins[bank].find(max_row);
          assert(bin_ptr != m_current_bins[bank].end());
          m_current_last_row[bank] = &(bin_ptr->second);
          
          //m_row_scoreboard[bank][max_row].score = 0;
          //m_row_scoreboard[bank][max_row].subscore = 0;
        } else { // there is no priority
          assert(0); // where did the request go???
        }
      } else {
        dram_req_t *req = m_current_queue[bank].back(); // JH : first request in queue in bank
        bin_ptr = m_current_bins[bank].find(req->row);
        assert(bin_ptr != m_current_bins[bank].end());  // where did the request go??
        m_current_last_row[bank] = &(bin_ptr->second); // JH : new active row
      }
      data_collection(bank);
      rowhit = false; // JH : row changed   
    } else { // row hit
      m_current_last_row[bank] = &(bin_ptr->second);
      // JH : current active row is the row which is the most recently issued row
      rowhit = true;
    }
  }
  // JH : request age++  (debug mode)
  std::list<dram_req_t *>::iterator it;
  for (it = m_current_queue[bank].begin(); it != m_current_queue[bank].end(); it++) {
    (*it)->age++;
  }

  std::list<dram_req_t *>::iterator next;
  dram_req_t *req;
  
  // JH : request priority
  std::list<std::list<dram_req_t *>::iterator>::iterator ip;
  std::list<std::list<dram_req_t *>::iterator>::iterator in; // score
  std::list<std::list<dram_req_t *>::iterator>::iterator im; // l2 score
  unsigned max_score = 0;
  unsigned max_l2_score = 0;
  unsigned max_l1_score = 0;
  unsigned max_prio = 0;
  for ( ip = m_current_last_row[bank]->begin(); ip != m_current_last_row[bank]->end(); ip++ ) {
    if (max_prio < (*(*ip))->prio) max_prio = (*(*ip))->prio;
    if ( max_score <= (*(*ip))->score ) {
      max_score = (*(*ip))->score;
      in = ip;
    }
    if (max_l2_score <= (*(*ip))->l2_score ) { // oldest urgent request first 
      max_l2_score = (*(*ip))->l2_score;
      im = ip;
    } 
  }

  if (m_config->frfcfs_option == WARPED_MC) {
    if (max_prio == 2) {  // oldest urgent request first
      next = (*im); // prio 2
      m_current_last_row[bank]->erase(im);
    } else { // no priority
      next = m_current_last_row[bank]->back();
      m_current_last_row[bank]->pop_back();
    }
  } else {
    next = m_current_last_row[bank]->back();
    if ((*next)->prio < max_prio || (*next)->l2_score < max_l2_score ) {
      m_dram->m_gpu->num_req_case++;
    } 
    if ((*next)->score < max_score) 
      m_dram->m_gpu->num_req_case_wa++;
    m_current_last_row[bank]->pop_back();
    // JH : scheduled request
  }
  req = (*next);
 
  // rowblp stats
  m_dram->access_num++;
  bool is_write = req->data->is_write();
  if (is_write)
    m_dram->write_num++;
  else
    m_dram->read_num++;

  if (rowhit) {
    m_dram->hits_num++;
    if (is_write)
      m_dram->hits_write_num++;
    else
      m_dram->hits_read_num++;
  }

  m_stats->concurrent_row_access[m_dram->id][bank]++;
  m_stats->row_access[m_dram->id][bank]++;
  m_current_queue[bank].erase(next); // JH : delete scheduled request

  if (m_current_last_row[bank]->empty()) { // row is empty
    m_current_bins[bank].erase(req->row);
    m_current_last_row[bank] = NULL;
  }
#ifdef DEBUG_FAST_IDEAL_SCHED
  if (req)
    printf("%08u : DRAM(%u) scheduling memory request to bank=%u, row=%u\n",
           (unsigned)gpu_sim_cycle, m_dram->id, req->bk, req->row);
#endif

  if (m_config->seperate_write_queue_enabled && req->data->is_write()) {
    assert(req != NULL && m_num_write_pending != 0);
    m_num_write_pending--;
  } else {
    assert(req != NULL && m_num_pending != 0);
    m_num_pending--;
  }

  return req;
}

void frfcfs_scheduler::print(FILE *fp) {
  for (unsigned b = 0; b < m_config->nbk; b++) {
    printf(" %u: queue length = %u\n", b, (unsigned)m_queue[b].size());
  }
}

void dram_t::scheduler_frfcfs() {
  unsigned mrq_latency;
  frfcfs_scheduler *sched = m_frfcfs_scheduler;

    while (!mrqq->empty()) {
    dram_req_t *req = mrqq->pop();

    // Power stats
    // if(req->data->get_type() != READ_REPLY && req->data->get_type() !=
    // WRITE_ACK)
    m_stats->total_n_access++;
    if (req->data->get_type() == WRITE_REQUEST) {
      m_stats->total_n_writes++;
    } else if (req->data->get_type() == READ_REQUEST) {
      m_stats->total_n_reads++;    
    }

    req->data->set_status(IN_PARTITION_MC_INPUT_QUEUE,
                          m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
    sched->add_req(req);
  }
  // JH 
  // update request age cycle
  if (/*m_config->frfcfs_option == DEFAULT ||*/ m_config->frfcfs_option == WARPED_MC ) {
    sched->update_request_age_cycle();
  }
  m_gpu->add_pending_req();
  dram_req_t *prio_req;

  dram_req_t *req;
  unsigned i;
  // JH
  if (m_config->frfcfs_option == WARPED_MC /*|| m_config->frfcfs_option == DEFAULT */) {
    for (i = 0; i < m_config->nbk; i++) { 
      if (!bk[i]->bk_prio) {
        prio_req = sched->schedule(i, bk[i]->curr_row);
        bk[i]->bk_prio = prio_req;
      }
    }
  }
  for (i = 0; i < m_config->nbk; i++) {
    if (m_config->frfcfs_option == WARPED_MC /*|| m_config->frfcfs_option == DEFAULT*/ ) {
      if (!bk[i]->mrq) { // JH : mrq == scheduled request
        req = bk[i]->bk_prio;
        if (req) {
          req->data->set_status(IN_PARTITION_MC_BANK_ARB_QUEUE,
                                m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
          //prio = (prio + 1) % m_config->nbk;
          bk[i]->mrq = req;
          if (m_config->gpgpu_memlatency_stat) { // JH : record latency stat
            mrq_latency = m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle -
                          bk[i]->mrq->timestamp;
            m_stats->tot_mrq_latency += mrq_latency;
            m_stats->tot_mrq_num++;
            bk[i]->mrq->timestamp =
                m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle;
            m_stats->mrq_lat_table[LOGB2(mrq_latency)]++;
            if (mrq_latency > m_stats->max_mrq_latency) {
              m_stats->max_mrq_latency = mrq_latency;
            }
          }
          //break;
        }
      }
    } else {
      unsigned b = (i + prio) % m_config->nbk; // JH : bank id
      if (!bk[b]->mrq) { // JH : mrq = scheduled request
        //req = bk[i]->bk_prio;
        req = sched->schedule(b, bk[b]->curr_row);
        if (req) {
          req->data->set_status(IN_PARTITION_MC_BANK_ARB_QUEUE,
                                m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
          prio = (prio + 1) % m_config->nbk;
          bk[b]->mrq = req;
          if (m_config->gpgpu_memlatency_stat) { // JH : record latency stat
            mrq_latency = m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle -
                          bk[b]->mrq->timestamp;
            m_stats->tot_mrq_latency += mrq_latency;
            m_stats->tot_mrq_num++;
            bk[b]->mrq->timestamp =
                m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle;
            m_stats->mrq_lat_table[LOGB2(mrq_latency)]++;
            if (mrq_latency > m_stats->max_mrq_latency) {
              m_stats->max_mrq_latency = mrq_latency;
            }
          }
          break;
        }
      }
    }
  }
}
      
