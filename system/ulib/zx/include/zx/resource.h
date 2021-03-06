// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <zx/handle.h>
#include <zx/object.h>

namespace zx {

class resource : public object<resource> {
public:
    static constexpr zx_obj_type_t TYPE = ZX_OBJ_TYPE_RESOURCE;

    constexpr resource() = default;

    explicit resource(zx_handle_t value) : object(value) {}

    explicit resource(handle&& h) : object(h.release()) {}

    resource(resource&& other) : object(other.release()) {}

    resource& operator=(resource&& other) {
        reset(other.release());
        return *this;
    }

    static zx_status_t create(const resource& parent, uint32_t kind, uint64_t low,
                              uint64_t high, resource* result);
};

using unowned_resource = const unowned<resource>;

} // namespace zx
