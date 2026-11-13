#pragma once

#include <QPainter>
#include <QProxyStyle>
#include <QStyleOption>

// Fusion places a group-box title above the outline. Center the title on the
// top edge and leave a gap in the stroke so the word is part of the frame.
class GroupBoxTitleStyle : public QProxyStyle {
public:
  using QProxyStyle::QProxyStyle;

  int styleHint(StyleHint hint, const QStyleOption *option,
                const QWidget *widget,
                QStyleHintReturn *returnData = nullptr) const override {
    if (hint == SH_GroupBox_TextLabelVerticalAlignment)
      return Qt::AlignVCenter;
    return QProxyStyle::styleHint(hint, option, widget, returnData);
  }

  QRect subControlRect(ComplexControl control,
                       const QStyleOptionComplex *option, SubControl subControl,
                       const QWidget *widget) const override {
    QRect rect =
        QProxyStyle::subControlRect(control, option, subControl, widget);
    if (control == CC_GroupBox && subControl == SC_GroupBoxLabel &&
        rect.left() < 8)
      rect.moveLeft(8);
    return rect;
  }

  void drawComplexControl(ComplexControl control,
                          const QStyleOptionComplex *option, QPainter *painter,
                          const QWidget *widget) const override {
    if (control != CC_GroupBox) {
      QProxyStyle::drawComplexControl(control, option, painter, widget);
      return;
    }
    const auto *groupBox =
        qstyleoption_cast<const QStyleOptionGroupBox *>(option);
    if (!groupBox) {
      QProxyStyle::drawComplexControl(control, option, painter, widget);
      return;
    }

    const QRect frame =
        subControlRect(CC_GroupBox, option, SC_GroupBoxFrame, widget);
    const QRect label =
        subControlRect(CC_GroupBox, option, SC_GroupBoxLabel, widget);
    if (groupBox->subControls & SC_GroupBoxFrame) {
      painter->save();
      if (!groupBox->text.isEmpty()) {
        QRegion region(groupBox->rect);
        region -= label.adjusted(-4, 0, 4, 0);
        painter->setClipRegion(region, Qt::IntersectClip);
      }
      QStyleOptionFrame frameOpt;
      frameOpt.QStyleOption::operator=(*groupBox);
      frameOpt.features = groupBox->features;
      frameOpt.lineWidth = groupBox->lineWidth;
      frameOpt.midLineWidth = groupBox->midLineWidth;
      frameOpt.rect = frame;
      drawPrimitive(PE_FrameGroupBox, &frameOpt, painter, widget);
      painter->restore();
    }

    if ((groupBox->subControls & SC_GroupBoxLabel) &&
        !groupBox->text.isEmpty()) {
      int alignment = int(groupBox->textAlignment & Qt::AlignHorizontal_Mask);
      if (alignment == 0)
        alignment = Qt::AlignLeft;
      alignment |= Qt::AlignVCenter | Qt::TextShowMnemonic;
      if (!styleHint(SH_UnderlineShortcut, option, widget))
        alignment |= Qt::TextHideMnemonic;
      drawItemText(painter, label, alignment, groupBox->palette,
                   groupBox->state & State_Enabled, groupBox->text,
                   QPalette::WindowText);
      if (groupBox->state & State_HasFocus) {
        QStyleOptionFocusRect focus;
        focus.QStyleOption::operator=(*groupBox);
        focus.rect = label;
        drawPrimitive(PE_FrameFocusRect, &focus, painter, widget);
      }
    }

    if (groupBox->subControls & SC_GroupBoxCheckBox) {
      QStyleOptionGroupBox check = *groupBox;
      check.subControls = SC_GroupBoxCheckBox;
      QProxyStyle::drawComplexControl(CC_GroupBox, &check, painter, widget);
    }
  }
};
