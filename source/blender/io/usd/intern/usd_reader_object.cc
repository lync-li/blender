/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

#include "usd_reader_object.h"
#include "usd_util.h"

#include "DNA_cachefile_types.h"
#include "DNA_constraint_types.h"
#include "DNA_modifier_types.h"
#include "DNA_space_types.h" /* for FILE_MAX */

#include "BKE_constraint.h"
#include "BKE_lib_id.h"
#include "BKE_modifier.h"
#include "BKE_object.h"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <iostream>

namespace blender::io::usd {

UsdObjectReader::UsdObjectReader(const pxr::UsdPrim &prim, const USDImporterContext &context)
    : prim_path_(""),
      prim_parent_name_(""),
      prim_name_(""),
      object_(nullptr),
      prim_(prim),
      context_(context),
      min_time_(std::numeric_limits<double>::max()),
      max_time_(std::numeric_limits<double>::min()),
      refcount_(0),
      parent_(nullptr),
      merged_with_parent_(false)
{
  prim_path_ = prim.GetPath().GetString();
  prim_name_ = prim.GetName().GetString();

  pxr::UsdPrim parent = prim.GetParent();
  prim_parent_name_ = parent ? parent.GetName().GetString() : prim_name_;
}

UsdObjectReader::~UsdObjectReader()
{
}

const pxr::UsdPrim &UsdObjectReader::prim() const
{
  return prim_;
}

Object *UsdObjectReader::object() const
{
  return object_;
}

void UsdObjectReader::setObject(Object *ob)
{
  object_ = ob;
}

struct Mesh *UsdObjectReader::read_mesh(struct Mesh *existing_mesh,
                                        double UNUSED(time),
                                        int UNUSED(read_flag),
                                        const char **UNUSED(err_str))
{
  return existing_mesh;
}

bool UsdObjectReader::topology_changed(Mesh * /*existing_mesh*/, double /*time*/)
{
  /* The default implementation of read_mesh() just returns the original mesh, so never changes the
   * topology. */
  return false;
}

void UsdObjectReader::setupObjectTransform(const double time)
{
  if (!this->object_) {
    return;
  }

  bool is_constant = false;
  float transform_from_usd[4][4];

  this->read_matrix(transform_from_usd, time, this->context_.import_params.scale, is_constant);

  /* Apply the matrix to the object. */
  BKE_object_apply_mat4(object_, transform_from_usd, true, false);
  BKE_object_to_mat4(object_, object_->obmat);

  /* TODO(makowalski):  Set up transform constraint if not constant. */
}

void UsdObjectReader::read_matrix(float r_mat[4][4] /* local matrix */,
                                  const double time,
                                  const float scale,
                                  bool &is_constant)
{
  pxr::UsdGeomXformable xformable(prim_);

  if (!xformable) {
    unit_m4(r_mat);
    is_constant = true;
    return;
  }

  /* TODO(makowalski):  Check for constant transform. */

  pxr::GfMatrix4d usd_local_xf;
  bool reset_xform_stack;
  xformable.GetLocalTransformation(&usd_local_xf, &reset_xform_stack, time);

  if (merged_with_parent_) {
    /* Take into account the parent's local xform. */
    pxr::UsdGeomXformable parent_xformable(prim_.GetParent());

    if (parent_xformable) {
      pxr::GfMatrix4d usd_parent_local_xf;
      parent_xformable.GetLocalTransformation(&usd_parent_local_xf, &reset_xform_stack, time);

      usd_local_xf = usd_parent_local_xf * usd_local_xf;
    }
  }

  double double_mat[4][4];
  usd_local_xf.Get(double_mat);

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      r_mat[i][j] = static_cast<float>(double_mat[i][j]);
    }
  }

  if (this->context_.stage_up_axis == pxr::UsdGeomTokens->y) {
    /* Swap the matrix from y-up to z-up. */
    copy_m44_axis_swap(r_mat, r_mat, USD_ZUP_FROM_YUP);
  }

  float scale_mat[4][4];
  scale_m4_fl(scale_mat, scale);
  mul_m4_m4m4(r_mat, scale_mat, r_mat);
}

double UsdObjectReader::minTime() const
{
  return min_time_;
}

double UsdObjectReader::maxTime() const
{
  return max_time_;
}

int UsdObjectReader::refcount() const
{
  return refcount_;
}

void UsdObjectReader::incref()
{
  refcount_++;
}

void UsdObjectReader::decref()
{
  refcount_--;
  BLI_assert(refcount_ >= 0);
}

} /* namespace blender::io::usd */