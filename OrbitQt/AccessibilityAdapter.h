// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_QT_GL_ACCESSIBILITY_H_
#define ORBIT_QT_GL_ACCESSIBILITY_H_

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <QAccessible>
#include <QAccessibleWidget>
#include <QWidget>

#include "OrbitBase/Logging.h"
#include "OrbitGlAccessibility.h"
#include "orbitglwidget.h"

namespace orbit_qt {

class A11yAdapter : public QAccessibleInterface {
 public:
  A11yAdapter() = delete;
  A11yAdapter(A11yAdapter& rhs) = delete;
  A11yAdapter(A11yAdapter&& rhs) = delete;

  static A11yAdapter* GetOrCreateAdapter(const orbit_gl::GlA11yControlInterface* iface);
  static void ClearAdapterCache();
  static void ReleaseAdapter(A11yAdapter* adapter);

  // check for valid pointers
  bool isValid() const override {
    bool result = info_ != nullptr && s_valid_adapters_.contains(this);
    CHECK(!result || s_iface_to_adapter_.find(info_)->second == this);
    return result;
  }
  QObject* object() const override { return nullptr; }

  // relations & interactions - currently not supported
  QVector<QPair<QAccessibleInterface*, QAccessible::Relation>> relations(
      QAccessible::Relation match = QAccessible::AllRelations) const override {
    return {};
  }
  QAccessibleInterface* focusChild() const override { return nullptr; }

  QAccessibleInterface* childAt(int x, int y) const override {
    return GetOrCreateAdapter(info_->AccessibleChildAt(x, y));
  }

  // navigation, hierarchy
  QAccessibleInterface* parent() const override {
    return GetOrCreateAdapter(info_->AccessibleParent());
  }
  QAccessibleInterface* child(int index) const override {
    return GetOrCreateAdapter(info_->AccessibleChild(index));
  }
  int childCount() const override { return info_->AccessibleChildCount(); }
  int indexOfChild(const QAccessibleInterface* child) const override;

  // properties and state
  QString text(QAccessible::Text t) const override { return info_->AccessibleName().c_str(); }
  void setText(QAccessible::Text t, const QString& text) override{};
  QRect rect() const override;
  QAccessible::Role role() const override {
    return static_cast<QAccessible::Role>(info_->AccessibleRole());
  }
  virtual QAccessible::State state() const override { return QAccessible::State(); }

 private:
  A11yAdapter(const orbit_gl::GlA11yControlInterface* info) : info_(info){};

  const orbit_gl::GlA11yControlInterface* info_ = nullptr;

  static absl::flat_hash_map<const orbit_gl::GlA11yControlInterface*, A11yAdapter*>
      s_iface_to_adapter_;
  static absl::flat_hash_set<A11yAdapter*> s_valid_adapters_;
};

class OrbitGlWidgetAccessible : public QAccessibleWidget {
 public:
  OrbitGlWidgetAccessible(OrbitGLWidget* widget);

  virtual QAccessibleInterface* childAt(int x, int y) const override;
  virtual QAccessibleInterface* child(int index) const;
  virtual int childCount() const;
  virtual int indexOfChild(const QAccessibleInterface* child);

 private:
  OrbitGLWidget* widget_ = nullptr;
};

QAccessibleInterface* GlAccessibilityFactory(const QString& classname, QObject* object);

}  // namespace orbit_qt

#endif