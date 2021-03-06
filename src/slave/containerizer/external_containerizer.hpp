/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __EXTERNAL_CONTAINERIZER_HPP__
#define __EXTERNAL_CONTAINERIZER_HPP__

#include <list>
#include <sstream>
#include <string>

#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/hashmap.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>
#include <stout/tuple.hpp>

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/isolator.hpp"
#include "slave/containerizer/launcher.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The scheme an external containerizer has to adhere to is;
//
// COMMAND < INPUT-PROTO > RESULT-PROTO
//
// launch < containerizer::Launch
// update < containerizer::Update
// usage < containerizer::Usage > mesos::ResourceStatistics
// wait < containerizer::Wait > containerizer::Termination
// destroy < containerizer::Destroy
//
// 'wait' on the external containerizer side is expected to block
// until the task command/executor has terminated.
//

// Check src/examples/python/test_containerizer.py for a rough
// implementation template of this protocol.

// TODO(tillt): Implement a protocol for external containerizer
// recovery by defining needed protobuf/s.
// Currently we expect to cover recovery entirely on the slave side.

// For debugging purposes of an external containerizer, it might be
// helpful to enable verbose logging on the slave (GLOG_v=2).

class ExternalContainerizerProcess;

class ExternalContainerizer : public Containerizer
{
public:
  ExternalContainerizer(const Flags& flags);

  virtual ~ExternalContainerizer();

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<Nothing> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> launch(
      const ContainerID& containerId,
      const TaskInfo& task,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID> > containers();

private:
  ExternalContainerizerProcess* process;
};


class ExternalContainerizerProcess
  : public process::Process<ExternalContainerizerProcess>
{
public:
  ExternalContainerizerProcess(const Flags& flags);

  // Recover containerized executors as specified by state. See
  // containerizer.hpp:recover for more.
  process::Future<Nothing> recover(const Option<state::SlaveState>& state);

  // Start the containerized executor.
  process::Future<Nothing> launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  // Update the container's resources.
  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  // Gather resource usage statistics for the containerized executor.
  process::Future<ResourceStatistics> usage(const ContainerID& containerId);

  // Get a future on the containerized executor's Termination.
  process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  // Terminate the containerized executor.
  void destroy(const ContainerID& containerId);

  // Get all active container-id's.
  process::Future<hashset<ContainerID> > containers();

private:
  // Startup flags.
  const Flags flags;

  // Information describing a container environment. A sandbox has to
  // be prepared before the external containerizer can be invoked.
  struct Sandbox
  {
    Sandbox(const std::string& directory, const Option<std::string>& user)
      : directory(directory), user(user) {}

    const std::string directory;
    const Option<std::string> user;
  };

  // Information describing a running container.
  struct Container
  {
    Container(const Sandbox& sandbox) : sandbox(sandbox), pid(None()) {}

    // Keep sandbox information available for subsequent containerizer
    // invocations.
    Sandbox sandbox;

    // External containerizer pid as per wait-invocation.
    // Wait should block on the external containerizer side, hence we
    // need to keep its pid for terminating if needed.
    Option<pid_t> pid;

    process::Promise<containerizer::Termination> termination;

    // As described in MESOS-1251, we need to make sure that events
    // that are triggered before launch has completed, are in fact
    // queued until then to reduce complexity within external
    // containerizer program implementations. To achieve that, we
    // simply queue all events onto this promise.
    process::Promise<Nothing> launched;

    Resources resources;
  };

  // Stores all active containers.
  hashmap<ContainerID, process::Owned<Container> > actives;

  process::Future<Nothing> _launch(
      const ContainerID& containerId,
      const process::Future<Option<int> >& future);

  void __launch(
      const ContainerID& containerId,
      const process::Future<Nothing>& future);

  process::Future<containerizer::Termination> _wait(
      const ContainerID& containerId);

  void __wait(
      const ContainerID& containerId,
      const process::Future<tuples::tuple<
          process::Future<Result<containerizer::Termination> >,
          process::Future<Option<int> > > >& future);

  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const Resources& resources);

  process::Future<Nothing> __update(
      const ContainerID& containerId,
      const process::Future<Option<int> >& future);

  process::Future<ResourceStatistics> _usage(
      const ContainerID& containerId);

  process::Future<ResourceStatistics> __usage(
      const ContainerID& containerId,
      const process::Future<tuples::tuple<
          process::Future<Result<ResourceStatistics> >,
          process::Future<Option<int> > > >& future);

  void _destroy(const ContainerID& containerId);

  void __destroy(
      const ContainerID& containerId,
      const process::Future<Option<int> >& future);

  // Abort a possibly pending "wait" in the external containerizer
  // process.
  void unwait(const ContainerID& containerId);

  // Call back for when the containerizer has terminated all processes
  // in the container.
  void cleanup(const ContainerID& containerId);

  Try<process::Subprocess> invoke(
      const std::string& command,
      const Sandbox& sandbox,
      const google::protobuf::Message& message,
      const std::map<std::string, std::string>& environment =
        (std::map<std::string, std::string>())); // Wrapped in parens due to: http://llvm.org/bugs/show_bug.cgi?id=13657
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __EXTERNAL_CONTAINERIZER_HPP__
