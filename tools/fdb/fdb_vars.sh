#!/bin/bash
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# Description: Variables for FoundationDB

#======================= MUST CUSTOMIZATION ====================================
# Data directories for FoundationDB storage
# Make sure to create these directories before running the script, and have to be absolute path.
# For simplicity, you can use one direcotry. For production, you should use SSDs.
# shellcheck disable=2034
DATA_DIRS="/mnt/foundationdb/data1,/mnt/foundationdb/data2,/mnt/foundationdb/data3"

MEDIUM_TYPE="SSD"

# Define the cluster IPs (comma-separated list of IP addresses)
# You should have at least 3 IP addresses for a production cluster
# The first IP addresses will be used as the coordinator,
# num of coordinators depends on the number of nodes, see the function get_coordinators.
# For high availability, machines should be in diffrent rack.
# shellcheck disable=2034
FDB_CLUSTER_IPS="172.200.0.5,172.200.0.6,172.200.0.7"

# Define the FoundationDB home directory, which contains the fdb binaries and logs.
# default is /fdbhome and have to be absolute path.
# shellcheck disable=2034
FDB_HOME="/fdbhome"

# Define the cluster id, shoule be generated random like mktemp -u XXXXXXXX,
# have to be different for each cluster.
# shellcheck disable=2034
FDB_CLUSTER_ID="ra7eOp7x"

# Define the cluster description, you 'd better to change it.
# shellcheck disable=2034
FDB_CLUSTER_DESC="mycluster"

# Define resource limits
# Memory limit in gigabytes
# shellcheck disable=2034
MEMORY_LIMIT_GB=64

# CPU cores limit
# shellcheck disable=2034
CPU_CORES_LIMIT=16

#======================= OPTIONAL CUSTOMIZATION ============================

# Define starting port for the servers
# This is the base port number for the fdbserver processes, usually does not need to be changed
# shellcheck disable=2034
FDB_PORT=4500

# Define the FoundationDB version
# shellcheck disable=2034
FDB_VERSION="7.1.38"

# Users who run the fdb processes, default is the current user
# shellcheck disable=2034
USER=$(whoami)

# ratio of storage, stateless and log process num in fdb
# shellcheck disable=2034
STORAGE_STATELESS_LOG_RATIO="2:1:1"

# Set process limits
# shellcheck disable=2034
STORAGE_PROCESSES_NUM_PER_HDD=1
# shellcheck disable=2034
LOG_PROCESSES_NUM_PER_HDD=1
# shellcheck disable=2034
STORAGE_PROCESSES_NUM_PER_SSD=4
# shellcheck disable=2034
LOG_PROCESSES_NUM_PER_SSD=4

# shellcheck disable=2034
MEMORY_STORAGE_GB=8
# shellcheck disable=2034
MEMORY_STATELESS_GB=1
# shellcheck disable=2034
MEMORY_LOG_GB=2