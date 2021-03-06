#pragma once

#include "source/common/protobuf/protobuf.h"

namespace Envoy {
namespace ProtobufMessage {

class ConstProtoVisitor {
public:
  virtual ~ConstProtoVisitor() = default;

  // Invoked when a field is visited, with the message, field descriptor and context. Returns a new
  // context for use when traversing the sub-message in a field.
  virtual const void* onField(const Protobuf::Message&, const Protobuf::FieldDescriptor&,
                              const void* ctxt) {
    return ctxt;
  }

  // Invoked when a message is visited, with the message and a context.
  virtual void onMessage(const Protobuf::Message&, const void*){};
};

void traverseMessage(ConstProtoVisitor& visitor, const Protobuf::Message& message,
                     const void* ctxt);

} // namespace ProtobufMessage
} // namespace Envoy
