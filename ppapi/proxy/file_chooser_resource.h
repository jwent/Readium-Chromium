// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PPAPI_PROXY_FILE_CHOOSER_RESOURCE_H_
#define PPAPI_PROXY_FILE_CHOOSER_RESOURCE_H_

#include <queue>
#include <string>
#include <vector>

#include "ppapi/proxy/plugin_resource.h"
#include "ppapi/proxy/ppapi_proxy_export.h"
#include "ppapi/shared_impl/array_writer.h"
#include "ppapi/shared_impl/tracked_callback.h"
#include "ppapi/thunk/ppb_file_chooser_api.h"

namespace ppapi {

struct PPB_FileRef_CreateInfo;

namespace proxy {

class PPAPI_PROXY_EXPORT FileChooserResource
    : public PluginResource,
      public NON_EXPORTED_BASE(thunk::PPB_FileChooser_API) {
 public:
  FileChooserResource(IPC::Sender* sender,
                      PP_Instance instance,
                      PP_FileChooserMode_Dev mode,
                      const std::string& accept_types);
  virtual ~FileChooserResource();

  // Resource overrides.
  virtual thunk::PPB_FileChooser_API* AsPPB_FileChooser_API() OVERRIDE;

  // PPB_FileChooser_API.
  virtual int32_t Show(const PP_ArrayOutput& output,
                       scoped_refptr<TrackedCallback> callback) OVERRIDE;
  virtual int32_t ShowWithoutUserGesture(
      PP_Bool save_as,
      PP_Var suggested_file_name,
      const PP_ArrayOutput& output,
      scoped_refptr<TrackedCallback> callback) OVERRIDE;
  virtual int32_t Show0_5(scoped_refptr<TrackedCallback> callback) OVERRIDE;
  virtual PP_Resource GetNextChosenFile() OVERRIDE;
  virtual int32_t ShowWithoutUserGesture0_5(
      PP_Bool save_as,
      PP_Var suggested_file_name,
      scoped_refptr<TrackedCallback> callback) OVERRIDE;

  // Parses the accept string into the given vector.
  static void PopulateAcceptTypes(const std::string& input,
                                  std::vector<std::string>* output);

 private:
  // PluginResource override.
  virtual void OnReplyReceived(int sequence,
                               int32_t result,
                               const IPC::Message& msg) OVERRIDE;

  void OnPluginMsgShowReply(
      const std::vector<ppapi::PPB_FileRef_CreateInfo>& chosen_files);

  int32_t ShowInternal(PP_Bool save_as,
                       const PP_Var& suggested_file_name,
                       scoped_refptr<TrackedCallback> callback);

  PP_FileChooserMode_Dev mode_;
  std::vector<std::string> accept_types_;

  // When using v0.6 of the API, contains the array output info.
  ArrayWriter output_;

  // When using v0.5 of the API, contains all files returned by the current
  // show callback that haven't yet been given to the plugin. The plugin will
  // repeatedly call us to get the next file, and we'll vend those out of this
  // queue, removing them when ownership has transferred to the plugin.
  std::queue<PP_Resource> file_queue_;

  scoped_refptr<TrackedCallback> callback_;

  DISALLOW_COPY_AND_ASSIGN(FileChooserResource);
};

}  // namespace proxy
}  // namespace ppapi

#endif  // PPAPI_PROXY_FILE_CHOOSER_RESOURCE_H_