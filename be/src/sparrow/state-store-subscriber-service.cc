// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#include "sparrow/state-store-subscriber-service.h"

#include <iostream>
#include <utility>

#include <boost/algorithm/string/join.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <concurrency/PosixThreadFactory.h>
#include <concurrency/Thread.h>
#include <concurrency/ThreadManager.h>
#include <glog/logging.h>
#include <gflags/gflags.h>
#include <protocol/TBinaryProtocol.h>
#include <transport/TBufferTransports.h>
#include <transport/TServerSocket.h>
#include <transport/TSocket.h>

#include "common/status.h"
#include "gen-cpp/StateStoreService_types.h"
#include "gen-cpp/StateStoreSubscriberService_types.h"

using namespace std;
using namespace boost;
using namespace ::apache::thrift;
using namespace ::apache::thrift::protocol;
using namespace ::apache::thrift::transport;
using namespace ::apache::thrift::server;
using impala::Status;
using impala::THostPort;
using impala::TStatusCode;

namespace sparrow {

const char* StateStoreSubscriber::DISCONNECTED_FROM_STATE_STORE_ERROR =
    "Client disconnected from state store";

StateStoreSubscriber::StateStoreSubscriber(const string& host, int port,
                                           const string& state_store_host,
                                           int state_store_port)
  : server_running_(false) {
  host_port_.host = host;
  host_port_.port = port;
  state_store_host_port_.host = state_store_host;
  state_store_host_port_.port = state_store_port;
}

StateStoreSubscriber::~StateStoreSubscriber() {
  if (!IsRunning()) {
    LOG(ERROR) << "StateStoreSubscriber destructed before being properly shut "
               << "down (using Stop())";
    // This assumes that Stop() cannot be called in a different thread.
    Stop();
  }
}

Status StateStoreSubscriber::RegisterService(const string& service_id,
                                             const THostPort& address) {
  RETURN_IF_ERROR(InitClient());

  TRegisterServiceRequest request;
  request.__set_subscriber_address(host_port_);
  request.__set_service_id(service_id);
  request.__set_service_address(address);
  TRegisterServiceResponse response;
  VLOG(1) << "Attempting to register service " << request.service_id
          << " on subscriber at " << request.subscriber_address.host << ":"
          << request.subscriber_address.port;

  try {
    client_->RegisterService(response, request);
  } catch (TTransportException& e) {
    // Client has gotten disconnected from the state store.
    Status status(e.what());
    status.AddErrorMsg(DISCONNECTED_FROM_STATE_STORE_ERROR);
    return status;
  }
  Status status(response.status);
  if (status.ok()) {
    services_.insert(service_id);
  }
  return status;
}

Status StateStoreSubscriber::UnregisterService(const string& service_id) {
  RETURN_IF_ERROR(InitClient());

  unordered_set<string>::iterator service = services_.find(service_id);
  if (service == services_.end()) {
    format error_status("Service id %1% not registered with this subscriber");
    error_status % service_id;
    return Status(error_status.str());
  }

  TUnregisterServiceRequest request;
  request.__set_subscriber_address(host_port_);
  request.__set_service_id(service_id);
  TUnregisterServiceResponse response;

  try {
    client_->UnregisterService(response, request);
  } catch ( TTransportException& e) {
    Status status(e.what());
    status.AddErrorMsg(DISCONNECTED_FROM_STATE_STORE_ERROR);
    return status;
  }
  Status status(response.status);
  if (status.ok()) {
    services_.erase(service);
  }
  return status;
}

Status StateStoreSubscriber::RegisterSubscription(
    const SubscriptionManager::UpdateCallback& update_callback,
    const unordered_set<string>& update_services, SubscriptionId* id) {
  RETURN_IF_ERROR(InitClient());

  TRegisterSubscriptionRequest request;
  request.__set_subscriber_address(host_port_);
  request.services.insert(update_services.begin(), update_services.end());
  request.__isset.services = true;
  TRegisterSubscriptionResponse response;
  VLOG(1) << "Attempting to register subscriber for services "
          << algorithm::join(update_services, ", ") << " at "
          << request.subscriber_address.host << ":" << request.subscriber_address.port;

  try {
    client_->RegisterSubscription(response, request);
  } catch (TTransportException& e) {
    // Client has gotten disconnected from the state store.
    Status status(e.what());
    status.AddErrorMsg(DISCONNECTED_FROM_STATE_STORE_ERROR);
    return status;
  }

  Status status(response.status);
  if (!status.ok()) {
    return status;
  }
  if (!response.__isset.subscription_id) {
    status.AddErrorMsg("Invalid response: subscription_id not set.");
    return status;
  }

  *id = response.subscription_id;
  lock_guard<mutex> lock(update_callbacks_lock_);
  update_callbacks_.insert(make_pair(response.subscription_id, update_callback));
  return status;
}

Status StateStoreSubscriber::UnregisterSubscription(SubscriptionId id) {
  RETURN_IF_ERROR(InitClient());

  {
    lock_guard<mutex> lock(update_callbacks_lock_);
    UpdateCallbacks::iterator callback = update_callbacks_.find(id);
    if (callback == update_callbacks_.end()) {
      format error_status("Subscription id %1% not registered with this subscriber");
      error_status % id;
      return Status(error_status.str());
    }
  }

  TUnregisterSubscriptionRequest request;
  request.__set_subscriber_address(host_port_);
  request.__set_subscription_id(id);
  TUnregisterSubscriptionResponse response;
  try {
    client_->UnregisterSubscription(response, request);
  } catch (TTransportException& e) {
    Status status(e.what());
    status.AddErrorMsg(DISCONNECTED_FROM_STATE_STORE_ERROR);
    return status;
  }

  Status status(response.status);
  if (status.ok()) {
    lock_guard<mutex> lock(update_callbacks_lock_);
    // Don't use the iterator to erase, because it may have been invalidated by now.
    update_callbacks_.erase(id);
  }
  return Status(response.status);
}

void StateStoreSubscriber::UpdateState(TUpdateStateResponse& response,
                                       const TUpdateStateRequest& request) {
  RETURN_IF_UNSET(request, service_memberships, response);

  ServiceStateMap state;
  StateFromThrift(request, &state);

  // Log all of the new state we just got.
  stringstream new_state;
  BOOST_FOREACH(const ServiceStateMap::value_type& service_state, state) {
    new_state << "State for service " << service_state.first << ":\n" << "Membership: ";
    BOOST_FOREACH(const Membership::value_type& instance,
                  service_state.second.membership) {
      new_state << instance.second.host << ":" << instance.second.port
                << " (at subscriber " << instance.first << "),";
    }
    new_state << "\n";
    // TODO: Log object updates here too, once we include them.
  }
  VLOG(1) << "Received new state:\n" << new_state.str();

  // Make a copy of update_callbacks_, to avoid problems if one of the callbacks
  // calls back into this StateStoreSubscriber, which may deadlock if we are holding
  // the update_callbacks_lock_.
  UpdateCallbacks update_callbacks_copy;
  {
    lock_guard<mutex> lock(update_callbacks_lock_);
    update_callbacks_copy = update_callbacks_;
  }

  // TODO: This is problematic if any of the callbacks take a long time. Eventually,
  // we'll probably want to execute the callbacks asynchronously in a different thread.
  BOOST_FOREACH(UpdateCallbacks::value_type& update, update_callbacks_copy) {
    update.second(state);
  }
  RETURN_AND_SET_STATUS_OK(response);
}

void StateStoreSubscriber::Start() {
  shared_ptr<TProcessor> processor(
      new StateStoreSubscriberServiceProcessor(shared_from_this()));
  shared_ptr<TServerTransport> server_transport(new TServerSocket(host_port_.port));
  shared_ptr<TTransportFactory> transport_factory(new TBufferedTransportFactory());
  shared_ptr<TProtocolFactory> protocol_factory(new TBinaryProtocolFactory());

  LOG(INFO) << "StateStoreSubscriber listening on " << host_port_.port;
  server_.reset(new TSimpleServer(processor, server_transport, transport_factory,
                                  protocol_factory));

  DCHECK(!server_running_);
  server_running_ = true;
  server_thread_.reset(new thread(&TSimpleServer::serve, server_));
}

bool StateStoreSubscriber::IsRunning() {
  return server_running_;
}

void StateStoreSubscriber::Stop() {
  Status status = InitClient();
  if (status.ok()) {
    try {
      // Unregister all running services.
      BOOST_FOREACH(const string& service_id, services_) {
        TUnregisterServiceRequest request;
        request.__set_subscriber_address(host_port_);
        request.__set_service_id(service_id);
        TUnregisterServiceResponse response;
        client_->UnregisterService(response, request);
        Status unregister_status(response.status);
        if (!unregister_status.ok()) {
          LOG(ERROR) << "Error when unregistering service " << service_id << ":"
                     << unregister_status.GetErrorMsg();
        }
      }

     lock_guard<mutex> lock(update_callbacks_lock_);
      // Unregister all subscriptions.
      BOOST_FOREACH(const UpdateCallbacks::value_type& callback_pair,
                    update_callbacks_) {
        SubscriptionId id = callback_pair.first;
        TUnregisterSubscriptionRequest request;
        request.__set_subscriber_address(host_port_);
        request.__set_subscription_id(id);
        TUnregisterSubscriptionResponse response;
        // Ignore any errors in the response.
        client_->UnregisterSubscription(response, request);
        Status unregister_status(response.status);
        if (!unregister_status.ok()) {
          string error_msg;
          unregister_status.GetErrorMsg(&error_msg);
          VLOG(1) << "Error when unregistering subscription " << id << ":"
                  << error_msg;
        }
      }
    } catch (TTransportException& e) {
      LOG(ERROR) << "Connection to state store disrupted when trying to unregister; "
                 << "received error: " << e.what();
    }
  }

  DCHECK(server_running_);
  server_->stop();
  server_running_ = false;
  // TODO: The server seems to keep going indefinitely if the subscriber still has a
  // connection to the state store, causing this to not return.  See if this is possible
  // to fix.
  //server_thread_->join();
}

Status StateStoreSubscriber::InitClient() {
  DCHECK(server_running_);

  Status status;
  if (client_.get() == NULL) {
    shared_ptr<TSocket> socket(new TSocket(state_store_host_port_.host,
                                           state_store_host_port_.port));
    shared_ptr<TTransport> transport(new TBufferedTransport(socket));
    shared_ptr<TProtocol> protocol(new TBinaryProtocol(transport));
    client_.reset(new StateStoreServiceClient(protocol));

    try {
      transport->open();
    } catch (TTransportException& e) {
      status.AddErrorMsg(e.what());
      status.AddErrorMsg("Unable to register with the StateStore");
    }
  }
  return status;
}

}
