/*
 * Copyright (C) 2013, Ragnar Hagg
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <iostream>
#include <fstream>
#include <string>
using namespace std;

#include <stdint.h>
#include <inttypes.h>
#include <assert.h>

#include "perfpirate.h"
#include "perf_data.h"
#include "perf_common.h"
#include "perf_pb.pb.h"


static PerfHeader header;
static fstream dumpfile;
static int n_pirates;
static int n_t_ctrs = 0;
static int n_p_ctrs = 0;

void
pb_ctr_fill(PerfCtrInfo *pb_ctr, ctr_t *ctr, const int id)
{
	pb_ctr->set_id(id);
	pb_ctr->set_name(ctr->event_name);
	pb_ctr->set_config(ctr->attr.config);
	pb_ctr->set_config1(ctr->attr.config1);
	pb_ctr->set_config2(ctr->attr.config2);
	pb_ctr->set_type(ctr->attr.type);
}

void
pb_initialize_pirate(pirate_conf_t *conf, pirate_pthread_conf_t *pth_conf,
			 		ctr_list_t *pirate_ctrs)
{	
	PerfHeader::PirateSetup *p_setup = header.mutable_p_setup();
	p_setup->set_ways(conf->ways);
	p_setup->set_cache_size(conf->size);
	p_setup->set_stride(conf->stride);
	p_setup->set_way_size(conf->way_size);
	p_setup->set_no_sweep(conf->no_sweep);
	p_setup->set_n_pirates(n_pirates);
	

	for (ctr_t *cur = pirate_ctrs->head; cur; cur = cur->next) {
		PerfCtrInfo *pb_cur = p_setup->add_ctr();
		pb_ctr_fill(pb_cur, cur, n_p_ctrs);
		n_p_ctrs++;
	}
	p_setup->set_n_ctrs(n_p_ctrs);

	for(int i = 0; i < n_pirates; i++)
		p_setup->add_cpu(pth_conf[i].cpu);

}

void
pb_initialize_target(const int cpu, const uint64_t sample_period, 
		ctr_list_t *perf_ctrs, char **exec_argv, const int exec_argc)
{
	PerfHeader::TargetSetup *t_setup = header.mutable_t_setup();

	t_setup->set_cpu(cpu);
	t_setup->set_sample_period(sample_period);

	for (ctr_t *cur = perf_ctrs->head; cur; cur = cur->next) {
		PerfCtrInfo *pb_cur = t_setup->add_ctr();
		pb_ctr_fill(pb_cur, cur, n_t_ctrs);
		n_t_ctrs++;
	}
	t_setup->set_n_ctrs(n_t_ctrs);


	std::string command = exec_argv[0];
	for(int i = 1; i < exec_argc; i++) {
		command += " ";
		command += exec_argv[i];
	}
	t_setup->set_command(command);

}

extern "C" void
pb_initialize(const int t_cpu, const int no_reference, 
		const uint64_t sample_period, ctr_list_t *perf_ctrs, 
		pirate_conf_t *conf, pirate_pthread_conf_t *pth_conf,
		const int numOf_pirates, ctr_list_t *pirate_ctrs, 
		const char *pb_output_name, char **exec_argv, const int exec_argc)
{

	n_pirates = numOf_pirates;
	pb_initialize_target(t_cpu, sample_period, perf_ctrs, 
							exec_argv, exec_argc);
	pb_initialize_pirate(conf, pth_conf, pirate_ctrs);

	header.set_no_reference(no_reference);

	dumpfile.open(pb_output_name, ios::out | ios::trunc | ios::binary);
	dumpfile << "PIRATEv1";

}

extern "C" void
pb_debugHeader(){
	// fstream debug_out("headerDebug.log", ios::out | ios::trunc);
	// debug_out << header.DebugString();
	// debug_out.close();
	cout << "**********************\n" << header.DebugString() << "**********************\n";
}


extern "C" void
pb_write_reference(read_format_t *r_data, int r_size){
	PerfCtrSample *ref = header.mutable_reference();
	ref->set_size(r_size);
	for(int i = 0; i < n_p_ctrs; i++)
		ref->add_ctr(r_data->ctr[i].val);
	assert(ref->ctr_size() == n_p_ctrs);
}

extern "C" void
pb_header2file()
{
	uint32_t size = header.ByteSize();
	dumpfile.write((char *)&size, sizeof(size));
	header.SerializeToOstream(&dumpfile);
	header.Clear();
}



extern "C" void
pb_dump_sample(read_format_t **data_array, int t_size, int p_size)
{	
	PerfCtrDump dump;
	
	PerfCtrSample *t_samp = dump.mutable_t_sample();

	t_samp->set_size(t_size);
	for(int i = 0; i < n_t_ctrs; i++)
		t_samp->add_ctr(data_array[0]->ctr[i].val);

	for(int j = 0; j < n_pirates; j++){
		PerfCtrSample *p_samp = dump.add_p_sample();
		p_samp->set_size(p_size);
		for(int i = 0; i < n_p_ctrs; i++)
			p_samp->add_ctr(data_array[j+1]->ctr[i].val);
	}

	uint32_t size = dump.ByteSize();
	dumpfile.write((char *)&size, sizeof(size));
	dump.SerializeToOstream(&dumpfile);
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * c-file-style: "k&r"
 * End:
 */
