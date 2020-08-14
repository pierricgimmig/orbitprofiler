// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_QT_ORBIT_DATA_VIEW_PANEL_H_
#define ORBIT_QT_ORBIT_DATA_VIEW_PANEL_H_

#include <QLineEdit>
#include <QWidget>

#include "orbittablemodel.h"
#include "orbittreeview.h"
#include "types.h"

namespace Ui {
class OrbitDataViewPanel;
}

class OrbitDataViewPanel : public QWidget {
  Q_OBJECT

 public:
  explicit OrbitDataViewPanel(QWidget* parent = nullptr);
  ~OrbitDataViewPanel() override;

  void Initialize(DataView* data_view, SelectionType selection_type, FontType font_type,
                  bool is_main_instance = true);
  void Link(OrbitDataViewPanel* a_Panel);
  void Refresh();
  void SetDataModel(DataView* model);
  void SetFilter(const QString& a_Filter);
  OrbitTreeView* GetTreeView();
  QLineEdit* GetFilterLineEdit();

 private slots:
  void on_FilterLineEdit_textEdited(const QString& a_Text);
  void on_refreshButton_clicked();

 private:
  Ui::OrbitDataViewPanel* ui;
};

#endif  // ORBIT_QT_ORBIT_DATA_VIEW_PANEL_H_
