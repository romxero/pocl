/* pocl-hsa.c - driver for HSA supported devices. Currently only AMDGCN.

   Copyright (c) 2015 Pekka Jääskeläinen / Tampere University of Technology
                 2015 Charles Chen <0charleschen0@gmail.com>

   Short snippets borrowed from the MatrixMultiplication example in
   the HSA runtime library sources (c) 2014 HSA Foundation Inc.

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/
/* Some example code snippets copied verbatim from vector_copy.c of HSA-Runtime-AMD: */
/* Copyright 2014 HSA Foundation Inc.  All Rights Reserved.
 *
 * HSAF is granting you permission to use this software and documentation (if
 * any) (collectively, the "Materials") pursuant to the terms and conditions
 * of the Software License Agreement included with the Materials.  If you do
 * not have a copy of the Software License Agreement, contact the  HSA Foundation for a copy.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE SOFTWARE.
 */

#include "hsa.h"
#include "hsa_ext_finalize.h"
#include "hsa_ext_amd.h"
#include "hsa_ext_image.h"

#include "pocl-hsa.h"
#include "common.h"
#include "devices.h"
#include "pocl_file_util.h"
#include "pocl_cache.h"
#include "pocl_llvm.h"
#include "pocl_util.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#ifndef _MSC_VER
#  include <sys/wait.h>
#  include <sys/time.h>
#  include <unistd.h>
#else
#  include "vccompat.hpp"
#endif

#define max(a,b) (((a) > (b)) ? (a) : (b))

/* TODO:
   - allocate buffers with hsa_memory_allocate() to ensure the driver
     works with HSA Base profile agents (assuming all memory is coherent
     requires the Full profile) -- or perhaps a separate hsabase driver
     for the simpler agents.
   - AMD SDK samples. Requires work mainly in these areas:
      - atomics support
      - image support
      - CL C++
   - OpenCL printf() support
   - get_global_offset() and global_work_offset param of clEnqueNDRker -
     HSA kernel dispatch packet has no offset fields -
     we must take care of it somewhere
   - clinfo of Ubuntu crashes
   - etc. etc.
*/

#define HSA_PROGRAM_CACHE_SIZE 32
#define HSA_KERNEL_CACHE_SIZE 64

/* for caching kernel dispatch data */
typedef struct hsa_kernel_cache_t {
  cl_kernel kernel;
  hsa_executable_t hsa_exe;
  uint64_t code_handle;
  uint32_t private_size;
  uint32_t static_group_size;
  hsa_signal_t kernel_completion_signal;
  void* kernargs;
  uint32_t args_segment_size;
} hsa_kernel_cache_t;

/* Simple statically-sized program/kernel data cache */
typedef struct hsa_program_cache_t {
  cl_program program;
  hsa_code_object_t code_object;
  /* Per-kernel data cache for dispatching. Must be inside program cache,
   * since we must destroy all kernels (hsa_executable_t) before
   * destroying a program */
  hsa_kernel_cache_t kernel_cache[HSA_KERNEL_CACHE_SIZE];
  unsigned kernel_cache_lastptr;
} hsa_program_cache_t;

typedef struct pocl_hsa_device_data {
  /* Currently loaded kernel. */
  cl_kernel current_kernel;
  /* The HSA kernel agent controlled by the device driver instance. */
  hsa_agent_t *agent;
  hsa_profile_t agent_profile;
  /* mem regions */
  hsa_region_t global_region, kernarg_region, group_region;
  /* Queue for pushing work to the agent. */
  hsa_queue_t *queue;
  /* Per-program data cache to simplify program compiling stage */
  hsa_program_cache_t program_cache[HSA_PROGRAM_CACHE_SIZE];
  unsigned program_cache_lastptr;
} pocl_hsa_device_data;

struct pocl_supported_hsa_device_properties
{
  char *dev_name;
  const char *llvm_cpu;
  const char *llvm_target_triplet;
  int has_64bit_long;
  cl_ulong max_mem_alloc_size;
  cl_ulong global_mem_size;
  cl_uint vendor_id;
  cl_device_mem_cache_type global_mem_cache_type;
  cl_uint global_mem_cacheline_size;
  cl_uint max_compute_units;
  cl_uint max_clock_frequency;
  cl_ulong max_constant_buffer_size;
  cl_ulong local_mem_size;
};

void
pocl_hsa_init_device_ops(struct pocl_device_ops *ops)
{
  pocl_basic_init_device_ops (ops);

  /* TODO: more descriptive name from HSA probing the device */
  ops->device_name = "hsa";
  ops->init_device_infos = pocl_hsa_init_device_infos;
  ops->probe = pocl_hsa_probe;
  ops->uninit = pocl_hsa_uninit;
  ops->init = pocl_hsa_init;
  ops->free = pocl_hsa_free;
  ops->compile_submitted_kernels = pocl_hsa_compile_submitted_kernels;
  ops->run = pocl_hsa_run;
  ops->read = pocl_basic_read;
  ops->read_rect = pocl_basic_read_rect;
  ops->write = pocl_basic_write;
  ops->write_rect = pocl_basic_write_rect;
  ops->copy = pocl_basic_copy;
  ops->copy_rect = pocl_basic_copy_rect;
  ops->get_timer_value = pocl_basic_get_timer_value;
}

#define MAX_HSA_AGENTS 16

static hsa_agent_t hsa_agents[MAX_HSA_AGENTS];
static hsa_agent_t* last_assigned_agent;
static int found_hsa_agents = 0;

static hsa_status_t
pocl_hsa_get_agents_callback(hsa_agent_t agent, void *data)
{
  hsa_device_type_t type;
  hsa_status_t stat = hsa_agent_get_info (agent, HSA_AGENT_INFO_DEVICE, &type);
  if (type != HSA_DEVICE_TYPE_GPU)
    {
      return HSA_STATUS_SUCCESS;
    }

  hsa_agent_feature_t features;
  stat = hsa_agent_get_info(agent, HSA_AGENT_INFO_FEATURE, &features);
  if (type != HSA_AGENT_FEATURE_KERNEL_DISPATCH)
    {
      return HSA_STATUS_SUCCESS;
    }


  hsa_agents[found_hsa_agents++] = agent;
  return HSA_STATUS_SUCCESS;
}

/*
 * Sets up the memory regions in pocl_hsa_device_data for a device
 */
static
hsa_status_t
setup_agent_memory_regions_callback(hsa_region_t region, void* data)
{
  struct pocl_hsa_device_data* d = (struct pocl_hsa_device_data*)data;

  hsa_region_segment_t segment;
  hsa_region_global_flag_t flags;
  hsa_region_get_info(region, HSA_REGION_INFO_SEGMENT, &segment);

  if (segment == HSA_REGION_SEGMENT_GLOBAL)
    {
      d->global_region = region;
      hsa_region_get_info(region, HSA_REGION_INFO_GLOBAL_FLAGS, &flags);
      if (flags & HSA_REGION_GLOBAL_FLAG_KERNARG)
        d->kernarg_region = region;
    }

  if (segment == HSA_REGION_SEGMENT_GROUP)
    d->group_region = region;

  return HSA_STATUS_SUCCESS;
}

//Get hsa-unsupported device features from hsa device list.
static int num_hsa_device = 1;

static struct _cl_device_id
supported_hsa_devices[MAX_HSA_AGENTS] =
{
  [0] =
  {
    .long_name = "Spectre",
    .llvm_cpu = NULL,                 // native: "kaveri",
    .llvm_target_triplet = "hsail64", // native: "amdgcn--amdhsa"
        .has_64bit_long = 1,
        .vendor_id = 0x1002,
        .global_mem_cache_type = CL_READ_WRITE_CACHE,
	.global_mem_cacheline_size = 64,
	.max_compute_units = 8,
	.max_clock_frequency = 720,
	.max_constant_buffer_size = 65536,
    .local_mem_type = CL_LOCAL,
    .endian_little = CL_TRUE,
    .preferred_wg_size_multiple = 64, // wavefront size on Kaveri
    .preferred_vector_width_char = 4,
    .preferred_vector_width_short = 2,
    .preferred_vector_width_int = 1,
    .preferred_vector_width_long = 1,
    .preferred_vector_width_float = 1,
    .preferred_vector_width_double = 1,
    .native_vector_width_char = 4,
    .native_vector_width_short = 2,
    .native_vector_width_int = 1,
    .native_vector_width_long = 1,
    .native_vector_width_float = 1,
    .native_vector_width_double = 1
  },
};

// Detect the HSA device and populate its properties to the device
// struct.
static void
get_hsa_device_features(char* dev_name, struct _cl_device_id* dev)
{

#define COPY_ATTR(ATTR) dev->ATTR = supported_hsa_devices[i].ATTR
#define COPY_VECWIDTH(ATTR) \
     dev->preferred_vector_width_ ## ATTR = supported_hsa_devices[i].preferred_vector_width_ ## ATTR; \
     dev->native_vector_width_ ## ATTR = supported_hsa_devices[i].native_vector_width_ ## ATTR;
  int found = 0;
  int i;
  for(i = 0; i < num_hsa_device; i++)
    {
      if (strcmp(dev_name, supported_hsa_devices[i].long_name) == 0)
        {
          COPY_ATTR (llvm_cpu);
          COPY_ATTR (llvm_target_triplet);
          COPY_ATTR (has_64bit_long);
          COPY_ATTR (vendor_id);
          COPY_ATTR (global_mem_cache_type);
          COPY_ATTR (global_mem_cacheline_size);
          COPY_ATTR (max_compute_units);
          COPY_ATTR (max_clock_frequency);
          COPY_ATTR (max_constant_buffer_size);
          COPY_ATTR (local_mem_type);
          COPY_ATTR (endian_little);
          COPY_ATTR (preferred_wg_size_multiple);
          COPY_VECWIDTH (char);
          COPY_VECWIDTH (short);
          COPY_VECWIDTH (int);
          COPY_VECWIDTH (long);
          COPY_VECWIDTH (float);
          COPY_VECWIDTH (double);
          found = 1;
          break;
        }
    }
  if (!found)
    POCL_ABORT("We found a device for which we don't have device"
               "OpenCL attribute information (compute unit count,"
               "constant buffer size etc), and there's no way to get"
               "the required stuff from HSA API. Please create a "
               "new entry with the information in supported_hsa_devices,"
               "and send a note/patch to pocl developers. Thanks!");
}

void
pocl_hsa_init_device_infos(struct _cl_device_id* dev)
{
  pocl_basic_init_device_infos (dev);
  dev->spmd = CL_TRUE;
  dev->autolocals_to_args = 0;

  assert(found_hsa_agents > 0);
  assert(last_assigned_agent < (hsa_agents + found_hsa_agents));
  dev->data = (void*)last_assigned_agent;
  hsa_agent_t agent = *last_assigned_agent++;

  uint32_t cache_sizes[4];
  hsa_status_t stat =
    hsa_agent_get_info (agent, HSA_AGENT_INFO_CACHE_SIZE,
                        &cache_sizes);
  // The only nonzero value on Kaveri is the first (L1)
  dev->global_mem_cache_size = cache_sizes[0];

  dev->short_name = dev->long_name = (char*)malloc (64*sizeof(char));
  stat = hsa_agent_get_info (agent, HSA_AGENT_INFO_NAME, dev->long_name);
  get_hsa_device_features (dev->long_name, dev);

  dev->type = CL_DEVICE_TYPE_GPU;

  dev->image_support = CL_FALSE;   // until it's actually implemented

  dev->single_fp_config = CL_FP_ROUND_TO_NEAREST | CL_FP_ROUND_TO_ZERO | CL_FP_ROUND_TO_INF | CL_FP_FMA | CL_FP_INF_NAN;
  dev->double_fp_config = CL_FP_ROUND_TO_NEAREST | CL_FP_ROUND_TO_ZERO | CL_FP_ROUND_TO_INF | CL_FP_FMA | CL_FP_INF_NAN;

  hsa_machine_model_t model;
  stat = hsa_agent_get_info (agent, HSA_AGENT_INFO_MACHINE_MODEL, &model);
  dev->address_bits = (model == HSA_MACHINE_MODEL_LARGE) ? 64 : 32;

  uint16_t wg_sizes[3];
  stat = hsa_agent_get_info(agent, HSA_AGENT_INFO_WORKGROUP_MAX_DIM, &wg_sizes);
  dev->max_work_item_sizes[0] = wg_sizes[0];
  dev->max_work_item_sizes[1] = wg_sizes[1];
  dev->max_work_item_sizes[2] = wg_sizes[2];

  stat = hsa_agent_get_info
    (agent, HSA_AGENT_INFO_WORKGROUP_MAX_SIZE, &dev->max_work_group_size);
  /*Image features*/
  hsa_dim3_t image_size;
  stat = hsa_agent_get_info (agent, HSA_EXT_AGENT_INFO_IMAGE_1D_MAX_ELEMENTS, &image_size);
  dev->image_max_buffer_size = image_size.x;
  stat = hsa_agent_get_info (agent, HSA_EXT_AGENT_INFO_IMAGE_2D_MAX_ELEMENTS, &image_size);
  dev->image2d_max_height = image_size.x;
  dev->image2d_max_width = image_size.y;
  stat = hsa_agent_get_info (agent, HSA_EXT_AGENT_INFO_IMAGE_3D_MAX_ELEMENTS, &image_size);
  dev->image3d_max_height = image_size.x;
  dev->image3d_max_width = image_size.y;
  dev->image3d_max_depth = image_size.z;
  // is this directly the product of the dimensions?
  //stat = hsa_agent_get_info(agent, ??, &dev->image_max_array_size);
  stat = hsa_agent_get_info
    (agent, HSA_EXT_AGENT_INFO_MAX_IMAGE_RD_HANDLES, &dev->max_read_image_args);
  stat = hsa_agent_get_info
    (agent, HSA_EXT_AGENT_INFO_MAX_IMAGE_RORW_HANDLES, &dev->max_write_image_args);
  stat = hsa_agent_get_info
    (agent, HSA_EXT_AGENT_INFO_MAX_SAMPLER_HANDLERS, &dev->max_samplers);
}

unsigned int
pocl_hsa_probe(struct pocl_device_ops *ops)
{
  int env_count = pocl_device_get_env_count(ops->device_name);

  POCL_MSG_PRINT_INFO("pocl-hsa: found %d env devices with %s.\n",
                      env_count, ops->device_name);

  /* No hsa env specified, the user did not request for HSA agents. */
  if (env_count <= 0)
    return 0;

  if (hsa_init() != HSA_STATUS_SUCCESS)
    {
      POCL_ABORT("pocl-hsa: hsa_init() failed.");
    }

  if (hsa_iterate_agents(pocl_hsa_get_agents_callback, NULL) !=
      HSA_STATUS_SUCCESS)
    {
      assert (0 && "pocl-hsa: could not get agents.");
    }
  POCL_MSG_PRINT_INFO("pocl-hsa: found %d agents.\n", found_hsa_agents);
  last_assigned_agent = hsa_agents;

  return found_hsa_agents;
}

static void hsa_queue_callback(hsa_status_t status, hsa_queue_t *q, void* data) {
  const char * sstr;
  hsa_status_string(status, &sstr);
  POCL_MSG_PRINT_INFO("HSA Device %s encountered an error in the HSA Queue: %s", (const char*)data, sstr);
}

void
pocl_hsa_init (cl_device_id device, const char* parameters)
{
  struct pocl_hsa_device_data *d;
  static int global_mem_id;
  static int first_hsa_init = 1;
  hsa_device_type_t dev_type;
  hsa_status_t status;

  if (first_hsa_init)
    {
      first_hsa_init = 0;
      global_mem_id = device->dev_id;
    }
  device->global_mem_id = global_mem_id;

  d = (struct pocl_hsa_device_data *) malloc (sizeof (struct pocl_hsa_device_data));
  memset(d, 0, sizeof(struct pocl_hsa_device_data));

  d->agent = (hsa_agent_t*)device->data;
  device->data = d;

  hsa_agent_iterate_regions (*d->agent, setup_agent_memory_regions_callback, d);

  uint32_t boolarg;
  status = hsa_region_get_info(d->global_region, HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED, &boolarg);
  assert(status == HSA_STATUS_SUCCESS);
  assert(boolarg != 0);

  size_t sizearg;
  status = hsa_region_get_info(d->global_region, HSA_REGION_INFO_ALLOC_MAX_SIZE, &sizearg);
  assert(status == HSA_STATUS_SUCCESS);
  device->max_mem_alloc_size = sizearg;

  /* For some reason, the global region size returned is 128 Terabytes...
   * for now, use the max alloc size, it seems to be a much more reasonable value.
  status = hsa_region_get_info(d->global_region, HSA_REGION_INFO_SIZE, &sizearg);
  assert(status == HSA_STATUS_SUCCESS);
  */
  device->global_mem_size = sizearg;

  status = hsa_region_get_info(d->group_region, HSA_REGION_INFO_SIZE, &sizearg);
  device->local_mem_size = sizearg;
  assert(status == HSA_STATUS_SUCCESS);

  status = hsa_region_get_info(d->global_region, HSA_REGION_INFO_RUNTIME_ALLOC_ALIGNMENT, &sizearg);
  device->mem_base_addr_align = sizearg * 8;

  status = hsa_agent_get_info(*d->agent, HSA_AGENT_INFO_PROFILE, &d->agent_profile);
  device->profile = ((d->agent_profile == HSA_PROFILE_FULL) ? "FULL_PROFILE" : "EMBEDDED_PROFILE");

  if (hsa_queue_create(*d->agent, 4, HSA_QUEUE_TYPE_MULTI, hsa_queue_callback, device->short_name,
                       -1, -1, &d->queue) != HSA_STATUS_SUCCESS)
    {
      POCL_ABORT("pocl-hsa: could not create the HSA queue.\n");
    }
}

void *
pocl_hsa_malloc (void *device_data, cl_mem_flags flags,
		    size_t size, void *host_ptr)
{
  void *b;

  if (flags & CL_MEM_COPY_HOST_PTR)
    {
      b = pocl_memalign_alloc(MAX_EXTENDED_ALIGNMENT, size);
      if (b != NULL)
	    {
	      memcpy(b, host_ptr, size);
	      hsa_memory_register(host_ptr, size);
	      return b;
	    }
      return NULL;
    }

  if (flags & CL_MEM_USE_HOST_PTR && host_ptr != NULL)
    {
      hsa_memory_register(host_ptr, size);
      return host_ptr;
    }
  b = pocl_memalign_alloc(MAX_EXTENDED_ALIGNMENT, size);
  if (b != NULL)
    return b;
  return NULL;
}

void
pocl_hsa_free (void *data, cl_mem_flags flags, void *ptr)
{
  if (flags & CL_MEM_USE_HOST_PTR)
  {
    return;
  }
  // TODO: hsa_memory_deregister() (needs size)
  POCL_MEM_FREE(ptr);
}

static void
setup_kernel_args (struct pocl_hsa_device_data *d,
                   _cl_command_node *cmd,
                   char *arg_space,
                   size_t max_args_size,
                   uint32_t *total_group_size)
{
  char *write_pos = arg_space;
  const char *last_pos = arg_space + max_args_size;

#define CHECK_SPACE(DSIZE)                                   \
  do {                                                       \
    if (write_pos + (DSIZE) > last_pos)                      \
      POCL_ABORT("pocl-hsa: too many kernel arguments!\n");  \
  } while (0)

  for (size_t i = 0; i < cmd->command.run.kernel->num_args; ++i)
    {
      struct pocl_argument *al = &(cmd->command.run.arguments[i]);
      if (cmd->command.run.kernel->arg_info[i].is_local)
        {
          CHECK_SPACE (sizeof (uint64_t));
          //For further info,
          //Please refer to https://github.com/HSAFoundation/HSA-Runtime-AMD/issues/8
          uint64_t temp = *total_group_size;
          memcpy(write_pos, &temp, sizeof(uint64_t));
          *total_group_size += (uint32_t)al->size;
          write_pos += sizeof(uint64_t);
        }
      else if (cmd->command.run.kernel->arg_info[i].type == POCL_ARG_TYPE_POINTER)
        {
          CHECK_SPACE (sizeof (uint64_t));
          /* Assuming the pointers are 64b (or actually the same as in
             host) due to HSA. TODO: the 32b profile. */

          if (al->value == NULL)
            {
              uint64_t temp = 0;
              memcpy (write_pos, &temp, sizeof (uint64_t));
            }
          else
            {
        	  uint64_t temp = (uint64_t)(*(cl_mem *)
				  (al->value))->device_ptrs[cmd->device->dev_id].mem_ptr;
              memcpy (write_pos, &temp, sizeof(uint64_t));
            }
          write_pos += sizeof(uint64_t);
#if 0
          /* TODO: It's legal to pass a NULL pointer to clSetKernelArguments. In
             that case we must pass the same NULL forward to the kernel.
             Otherwise, the user must have created a buffer with per device
             pointers stored in the cl_mem. */
          if (al->value == NULL)
            {
              arguments[i] = malloc (sizeof (void *));
              *(void **)arguments[i] = NULL;
            }
          else
            arguments[i] =
              &((*(cl_mem *) (al->value))->device_ptrs[cmd->device->dev_id].mem_ptr);
#endif
        }
      else if (cmd->command.run.kernel->arg_info[i].type == POCL_ARG_TYPE_IMAGE)
        {
          POCL_ABORT_UNIMPLEMENTED("pocl-hsa: image arguments not implemented.\n");
        }
      else if (cmd->command.run.kernel->arg_info[i].type == POCL_ARG_TYPE_SAMPLER)
        {
          POCL_ABORT_UNIMPLEMENTED("pocl-hsa: sampler arguments not implemented.\n");
        }
      else
        {
          // Scalars.
          CHECK_SPACE (al->size);
          memcpy (write_pos, al->value, al->size);
          write_pos += al->size;
        }
    }

#if 0
  for (size_t i = cmd->command.run.kernel->num_args;
       i < cmd->command.run.kernel->num_args + cmd->command.run.kernel->num_locals;
       ++i)
    {
      POCL_ABORT_UNIMPLEMENTED("hsa: automatic local buffers not implemented.");
      al = &(cmd->command.run.arguments[i]);
      arguments[i] = malloc (sizeof (void *));
      *(void **)(arguments[i]) = pocl_hsa_malloc (data, 0, al->size, NULL);
    }
#endif
}

/* Sets up things that go into kernel dispatch packet and are cacheable.
 * If stuff is cached, returns d->program_cache[i].kernel_cache[j]
 * If stuff is not cached, and d->program_cache[i].kernel_cache is not full,
 * puts things there;
 * otherwise puts things into 'stack_cache' argument;
 * returns a pointer to the actually used storage */
static hsa_kernel_cache_t* cache_kernel_dispatch_data(cl_kernel kernel,
                                pocl_hsa_device_data* d,
                                hsa_code_object_t* code_object,
                                hsa_kernel_cache_t *stack_cache) {
  hsa_program_cache_t* p = NULL;
  hsa_kernel_cache_t* out = NULL;

  assert(code_object != NULL);
  assert(stack_cache != NULL);
  assert(d != NULL);

  for (unsigned i = 0; i<HSA_PROGRAM_CACHE_SIZE; i++)
    {
      if (memcmp(&d->program_cache[i].code_object, code_object,
                 sizeof(hsa_code_object_t)) == 0)
        {
          p = &d->program_cache[i];
          for (unsigned j = 0; j<HSA_KERNEL_CACHE_SIZE; j++)
            {
              if (p->kernel_cache[j].kernel == kernel)
                return &p->kernel_cache[j];
            }
          if (p->kernel_cache_lastptr < HSA_KERNEL_CACHE_SIZE)
            out = &p->kernel_cache[p->kernel_cache_lastptr++];
          else
            out = stack_cache;
        }
    }

  if (!p)
    out = stack_cache;

  hsa_status_t status = hsa_executable_create (HSA_PROFILE_FULL,
                                  HSA_EXECUTABLE_STATE_UNFROZEN,
                                  "", &out->hsa_exe);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: error while creating an executable.\n");

  status = hsa_executable_load_code_object (out->hsa_exe, *d->agent,
                                            *code_object, "");
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: error while loading the code object into executable.\n");

  status = hsa_executable_freeze (out->hsa_exe, NULL);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: error while freezing the executable.\n");

  hsa_executable_symbol_t kernel_symbol;

  size_t kernel_name_length = strlen (kernel->name);
  char *symbol = malloc (kernel_name_length + 2);
  symbol[0] = '&';
  symbol[1] = '\0';

  strncat (symbol, kernel->name, kernel_name_length);

  POCL_MSG_PRINT_INFO("pocl-hsa: getting kernel symbol %s.\n", symbol);

  status = hsa_executable_get_symbol
    (out->hsa_exe, NULL, symbol, *d->agent, 0, &kernel_symbol);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: unable to get the kernel function symbol\n");

  hsa_symbol_kind_t symtype;
  status = hsa_executable_symbol_get_info
    (kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_TYPE, &symtype);
  if(symtype != HSA_SYMBOL_KIND_KERNEL)
    POCL_ABORT ("pocl-hsa: the kernel function symbol resolves to something else than a function\n");

  uint64_t code_handle;
  status = hsa_executable_symbol_get_info
    (kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &code_handle);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: unable to get the code handle for the kernel function.\n");

  out->code_handle = code_handle;

  status = hsa_executable_symbol_get_info
    (kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE, &out->static_group_size);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: unable to get the group segment size for the kernel function.\n");

  status = hsa_executable_symbol_get_info
    (kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE, &out->private_size);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: unable to get the private segment size for the kernel function.\n");

  hsa_signal_value_t initial_value = 1;
  status = hsa_signal_create(initial_value, 0, NULL, &out->kernel_completion_signal);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: unable to create a signal.\n");

  status = hsa_executable_symbol_get_info
    (kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,
     &out->args_segment_size);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: unable to get required memory size for kernel args.\n");

  status = hsa_memory_allocate(d->kernarg_region, out->args_segment_size, &out->kernargs);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: unable to allocate memory for kernel args.\n");

  out->kernel = (cl_kernel)kernel;
  return out;
}

void
pocl_hsa_run(void *dptr, _cl_command_node* cmd)
{
  struct pocl_hsa_device_data *d;
  unsigned i;
  cl_kernel kernel = cmd->command.run.kernel;
  struct pocl_context *pc = &cmd->command.run.pc;
  hsa_kernel_dispatch_packet_t *kernel_packet;
  hsa_signal_t kernel_completion_signal;
  hsa_region_t region;
  hsa_kernel_cache_t stack_cache, *cached_data;
  int status;

  assert (dptr != NULL);
  d = dptr;
  d->current_kernel = kernel;

  hsa_code_object_t *co = (hsa_code_object_t*)cmd->command.run.device_data;

  cached_data = cache_kernel_dispatch_data(kernel, d, co, &stack_cache);

  free(co);

  const uint32_t queueMask = d->queue->size - 1;

  uint64_t queue_index =
    hsa_queue_load_write_index_relaxed (d->queue);
  kernel_packet =
    &(((hsa_kernel_dispatch_packet_t*)(d->queue->base_address))[queue_index & queueMask]);

  kernel_packet->setup |= (uint16_t)cmd->command.run.pc.work_dim << HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;

  /* Process the kernel arguments. Convert the opaque buffer
     pointers to real device pointers, allocate dynamic local
     memory buffers, etc. */

  kernel_packet->workgroup_size_x = cmd->command.run.local_x;
  kernel_packet->workgroup_size_y = cmd->command.run.local_y;
  kernel_packet->workgroup_size_z = cmd->command.run.local_z;

  kernel_packet->grid_size_x = kernel_packet->grid_size_y = kernel_packet->grid_size_z = 1;
  kernel_packet->grid_size_x = pc->num_groups[0] * cmd->command.run.local_x;
  kernel_packet->grid_size_y = pc->num_groups[1] * cmd->command.run.local_y;
  kernel_packet->grid_size_z = pc->num_groups[2] * cmd->command.run.local_z;

  kernel_packet->kernel_object = cached_data->code_handle;
  kernel_packet->private_segment_size = cached_data->private_size;
  uint32_t total_group_size = cached_data->static_group_size;

  /* Reset the signal (it might be cached) to its initial value of 1 */
  hsa_signal_value_t initial_value = 1;
  hsa_signal_store_relaxed(cached_data->kernel_completion_signal, initial_value);
  kernel_packet->completion_signal = cached_data->kernel_completion_signal;

  setup_kernel_args (d, cmd, (char*)cached_data->kernargs, cached_data->args_segment_size, &total_group_size);

  kernel_packet->group_segment_size = total_group_size;

  POCL_MSG_PRINT_INFO("pocl-hsa: kernel's total group size: %u\n", total_group_size);
  if (total_group_size > cmd->device->local_mem_size)
    POCL_ABORT ("pocl-hsa: required local memory > device local memory!\n");

  kernel_packet->kernarg_address = cached_data->kernargs;

  uint64_t header = 0;
  header |= HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_ACQUIRE_FENCE_SCOPE;
  header |= HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_RELEASE_FENCE_SCOPE;
  header |= HSA_PACKET_TYPE_KERNEL_DISPATCH << HSA_PACKET_HEADER_TYPE;
   __atomic_store_n((uint16_t*)(&kernel_packet->header), header, __ATOMIC_RELEASE);

   /*
    * Increment the write index and ring the doorbell to dispatch the kernel.
    */
   hsa_queue_store_write_index_relaxed (d->queue, queue_index + 1);
   hsa_signal_store_relaxed (d->queue->doorbell_signal, queue_index);

  /* Launch the kernel by allocating a slot in the queue, writing the
     command to it, signaling the update with a door bell and finally,
     block waiting until finish signalled with the completion_signal. */

  hsa_signal_value_t sigval =
    hsa_signal_wait_acquire
    (cached_data->kernel_completion_signal, HSA_SIGNAL_CONDITION_LT, 1,
     (uint64_t)(-1), HSA_WAIT_STATE_ACTIVE);

  /* if the cache is full, release stuff */
  if (cached_data == &stack_cache)
    {
      hsa_executable_destroy(cached_data->hsa_exe);
      hsa_signal_destroy(cached_data->kernel_completion_signal);
    }

  /* TODO this */
  for (i = 0; i < kernel->num_args; ++i)
    {
      if (kernel->arg_info[i].is_local)
        {
#if 0
          pocl_hsa_free (data, 0, *(void **)(arguments[i]));
          POCL_MEM_FREE(arguments[i]);
#endif
        }
      else if (kernel->arg_info[i].type == POCL_ARG_TYPE_IMAGE)
        {
#if 0
          pocl_hsa_free (data, 0, *(void **)(arguments[i]));
          POCL_MEM_FREE(arguments[i]);
#endif
        }
#if 0
      else if (kernel->arg_info[i].type == POCL_ARG_TYPE_SAMPLER ||
               (kernel->arg_info[i].type == POCL_ARG_TYPE_POINTER &&
                *(void**)args->kernel_args[i] == NULL))
        {
          POCL_MEM_FREE(arguments[i]);
        }
#endif
    }
  for (i = kernel->num_args;
       i < kernel->num_args + kernel->num_locals;
       ++i)
    {
#if 0
      pocl_hsa_free(data, 0, *(void **)(arguments[i]));
      POCL_MEM_FREE(arguments[i]);
#endif
    }
}

static int compile_parallel_bc_to_brig(const char* tmpdir, char* brigfile) {
  int error;
  char hsailfile[POCL_FILENAME_LENGTH];
  char bytecode[POCL_FILENAME_LENGTH];
  char command[4096];

  error = snprintf (bytecode, POCL_FILENAME_LENGTH,
                    "%s%s", tmpdir, POCL_PARALLEL_BC_FILENAME);
  assert (error >= 0);

  error = snprintf (brigfile, POCL_FILENAME_LENGTH,
                    "%s%s.brig", tmpdir, POCL_PARALLEL_BC_FILENAME);
  assert (error >= 0);

  if (pocl_exists(brigfile))
    POCL_MSG_PRINT_INFO("pocl-hsa: using existing BRIG file: \n%s\n", brigfile);
  else
    {
      POCL_MSG_PRINT_INFO("pocl-hsa: BRIG file not found, compiling parallel.bc to brig file: \n%s\n", bytecode);

      // TODO call llvm via c++ interface like pocl_llvm_codegen()
      error = snprintf (hsailfile, POCL_FILENAME_LENGTH,
                    "%s%s.hsail", tmpdir, POCL_PARALLEL_BC_FILENAME);
      assert (error >= 0);

      error = snprintf (command, 4096, LLC " -O2 -march=hsail64 -filetype=asm -o %s %s", hsailfile, bytecode);
      assert (error >= 0);
      error = system(command);
      if (error != 0)
        {
          POCL_MSG_PRINT_INFO("pocl-hsa: llc exit status %i\n", WEXITSTATUS(error));
          return error;
        }

      error = snprintf (command, 4096, HSAIL_ASM " -o %s %s", brigfile, hsailfile);
      assert (error >= 0);
      error = system(command);
      if (error != 0)
        {
          POCL_MSG_PRINT_INFO("pocl-hsa: HSAILasm exit status %i\n", WEXITSTATUS(error));
          return error;
        }
    }

  return 0;
}

void
pocl_hsa_compile_submitted_kernels (_cl_command_node *cmd)
{
  if (cmd->type != CL_COMMAND_NDRANGE_KERNEL)
    return;

  int error;
  hsa_status_t status;
  char brigfile[POCL_FILENAME_LENGTH];
  char *brig_blob;

  cl_program program = cmd->command.run.kernel->program;

  struct pocl_hsa_device_data *d =
    (struct pocl_hsa_device_data*)cmd->device->data;

  hsa_code_object_t *out = malloc(sizeof(hsa_code_object_t));
  cmd->command.run.device_data = (void**)out;

  for (unsigned i = 0; i<HSA_PROGRAM_CACHE_SIZE; i++)
    if (d->program_cache[i].program == program)
      {
        *out = d->program_cache[i].code_object;
        return;
      }

  if (compile_parallel_bc_to_brig(cmd->command.run.tmp_dir, brigfile))
    POCL_ABORT("Compiling LLVM IR -> HSAIL -> BRIG failed.\n");

  POCL_MSG_PRINT_INFO("pocl-hsa: loading binary from file %s.\n", brigfile);
  uint64_t filesize = 0;
  int read = pocl_read_file(brigfile, &brig_blob, &filesize);

  if (read != 0)
    POCL_ABORT("pocl-hsa: could not read the binary.\n");

  POCL_MSG_PRINT_INFO("pocl-hsa: BRIG binary size: %lu.\n", filesize);

  hsa_ext_module_t hsa_module = (hsa_ext_module_t)brig_blob;

  hsa_ext_program_t hsa_program;
  memset (&hsa_program, 0, sizeof (hsa_ext_program_t));

  status = hsa_ext_program_create
    (HSA_MACHINE_MODEL_LARGE, HSA_PROFILE_FULL, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, NULL,
     &hsa_program);

  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: error while building the HSA program.\n");

  status = hsa_ext_program_add_module (hsa_program, hsa_module);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: error while adding the BRIG module to the HSA program.\n");

  hsa_isa_t isa;
  status = hsa_agent_get_info (*d->agent, HSA_AGENT_INFO_ISA, &isa);

  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: error while getting the ISA info for the HSA AGENT.\n");

  hsa_ext_control_directives_t control_directives;
  memset (&control_directives, 0, sizeof (hsa_ext_control_directives_t));

  hsa_code_object_t code_object;
  status = hsa_ext_program_finalize
    (hsa_program, isa, 0, control_directives, "",
     HSA_CODE_OBJECT_TYPE_PROGRAM, &code_object);

  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: error finalizing the program.\n");

  status = hsa_ext_program_destroy (hsa_program);
  if (status != HSA_STATUS_SUCCESS)
    POCL_ABORT ("pocl-hsa: error destroying the program.\n");

  free(brig_blob);
  *out = code_object;

  if (d->program_cache_lastptr < HSA_PROGRAM_CACHE_SIZE)
    {
      d->program_cache[d->program_cache_lastptr].code_object = code_object;
      d->program_cache[d->program_cache_lastptr++].program = program;
    }

}

void
pocl_hsa_uninit (cl_device_id device)
{
  struct pocl_hsa_device_data *d = (struct pocl_hsa_device_data*)device->data;

  for (unsigned j = 0; j < HSA_PROGRAM_CACHE_SIZE; j++)
    {
      if (d->program_cache[j].program)
        {
          hsa_program_cache_t *cache = &d->program_cache[j];
          for (unsigned i = 0; i < HSA_KERNEL_CACHE_SIZE; i++)
            if (cache->kernel_cache[i].kernel)
              {
                hsa_executable_destroy(cache->kernel_cache[i].hsa_exe);
                hsa_signal_destroy(cache->kernel_cache[i].kernel_completion_signal);
              }
          hsa_code_object_destroy(cache->code_object);
        }
    }

  hsa_queue_destroy(d->queue);
  POCL_MEM_FREE(d);
  device->data = NULL;
}
