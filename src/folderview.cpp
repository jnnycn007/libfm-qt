/*
 * Copyright (C) 2012 - 2015  Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "folderview.h"
#include "foldermodel.h"
#include <QHeaderView>
#include <QVBoxLayout>
#include <QContextMenuEvent>
#include "proxyfoldermodel.h"
#include "folderitemdelegate.h"
#include "dndactionmenu.h"
#include "filemenu.h"
#include "foldermenu.h"
#include "filelauncher.h"
#include "fileoperation.h"
#include "utilities.h"
#include <QTimer>
#include <QDate>
#include <QDebug>
#include <QClipboard>
#include <QMimeData>
#include <QHoverEvent>
#include <QApplication>
#include <QPainter>
#include <QScrollBar>
#include <QMetaType>
#include <QMessageBox>
#include <QLineEdit>
#include <QTextEdit>
#include <QWidgetAction> // for detailed list header context menu
#include <QLabel> // for detailed list header context menu
#include <xcb/xcb.h> // for XDS support
#include "xdndworkaround.h" // for XDS support
#include "folderview_p.h"

#include <cmath>
#include <algorithm>

#define SCROLL_FRAMES_PER_SEC 50
#define SCROLL_DURATION 300 // in ms

static const int scrollAnimFrames = SCROLL_FRAMES_PER_SEC * SCROLL_DURATION / 1000;

using namespace Fm;

FolderViewListView::FolderViewListView(QWidget* parent):
    QListView(parent),
    activationAllowed_(true),
    cursorOnSelectionCorner_(false),
    mouseLeftPressed_(false) {
    connect(this, &QListView::activated, this, &FolderViewListView::activation);
    // inline renaming
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMouseTracking(true); // needed with selection corner icon

    // for smooth scrolling (it is Qt's default)
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);


    viewport()->setAcceptDrops(true);
    /* If the list view is already visible, setMovement() will lay out items again with delay
       (see Qt, QListView::setMovement(), d->doDelayedItemsLayout()) and thus drop events will
       remain disabled for its viewport. So, we should call it here, before showing the view. */
    setMovement(QListView::Static);
}

FolderViewListView::~FolderViewListView() {
}

void FolderViewListView::startDrag(Qt::DropActions supportedActions) {
    mouseLeftPressed_ = false; // see FolderViewListView::mouseMoveEvent
    if(movement() != Static) {
        QListView::startDrag(supportedActions);
    }
    else {
        QAbstractItemView::startDrag(supportedActions);
    }
}

QItemSelectionModel::SelectionFlags FolderViewListView::selectionCommand(const QModelIndex& index, const QEvent* event) const {
    // select/deselect the item by pressing its selection corner with the left mouse button
    if(cursorOnSelectionCorner_
       && event != nullptr
       && event->type() == QEvent::MouseButtonPress
       && event->isSinglePointEvent()) {
        auto e = static_cast<const QSinglePointEvent*>(event);
        if(e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ShiftModifier)) {
            return QItemSelectionModel::Toggle;
        }
    }
    return QListView::selectionCommand(index, event);
}

void FolderViewListView::mousePressEvent(QMouseEvent* event) {
    if(event->buttons() == Qt::LeftButton) { // see FolderViewListView::mouseMoveEvent
        mouseLeftPressed_ = true;
        if(indexAt(event->position().toPoint()).isValid()) {
            globalItemPressPoint_ = event->globalPosition().toPoint();
        }
        else {
            globalItemPressPoint_ = QPoint();
        }
    }

    QListView::mousePressEvent(event);

    if(selectionMode() == QAbstractItemView::ExtendedSelection
       && (event->modifiers() & Qt::ShiftModifier)) {
        // NOTE: Qt sometimes does not respect the current item sorting with a Shift selection.
        // That seems like a problem in QListView. As a workaround, the selection is sorted here.
        auto selModel = selectionModel();
        auto sel = selModel->selection();
        if(!sel.isEmpty()) {
            std::sort(sel.begin(), sel.end(), [](QItemSelectionRange a, QItemSelectionRange b) {
                return a.top() < b.top();
            });
            selModel->select(sel, QItemSelectionModel::SelectCurrent);
        }
    }

    static_cast<FolderView*>(parent())->childMousePressEvent(event);
}

void FolderViewListView::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() == Qt::NoButton
        // NOTE: Filter the BACK & FORWARD buttons to not Drag & Drop with them.
        // (by default Qt views drag with any button)
        || ((event->buttons() & ~(Qt::BackButton | Qt::ForwardButton))
            && !(event->buttons() == Qt::LeftButton
                    // don't draw rubberband if left mouse button isn't pressed inside view
                    // (this event may have been sent by FolderView::scrollSmoothly)
                && (!mouseLeftPressed_
                    // don't start drag if the cursor isn't moved since pressing left mouse button on an item
                    // (because the user may want to scroll the view with mouse wheel before dragging)
                    || (globalItemPressPoint_ - event->globalPosition().toPoint()).manhattanLength() <= QApplication::startDragDistance())))) {
        bool cursorOnSelectionCorner = cursorOnSelectionCorner_;
        QListView::mouseMoveEvent(event);
        // update the index if the cursor enters/leaves the selection corner icon
        if(cursorOnSelectionCorner != cursorOnSelectionCorner_ && event->buttons() == Qt::NoButton) {
            update(indexAt(event->position().toPoint()));
        }
    }
}

QModelIndex FolderViewListView::indexAt(const QPoint& point) const {
    QModelIndex index = QListView::indexAt(point);
    bool isCursorPos(point == viewport()->mapFromGlobal(QCursor::pos()));
    if(isCursorPos) {
        cursorOnSelectionCorner_ = false;
    }
    // NOTE: QListView has a severe design flaw here. It does hit-testing based on the
    // total bound rect of the item. The width of an item is determined by max(icon_width, text_width).
    // So if the text label is much wider than the icon, when you click outside the icon but
    // the point is still within the outer bound rect, the item is still selected.
    // This results in very poor usability. Let's do precise hit-testing here.
    // An item is hit only when the point is in the icon or text label.
    // If the point is in the bound rectangle but outside the icon or text, it should not be selected.
    if(viewMode() == QListView::IconMode && index.isValid()) {
        QRect visRect = visualRect(index); // visible area on the screen
        FolderItemDelegate* delegate = static_cast<FolderItemDelegate*>(itemDelegateForColumn(FolderModel::ColumnFileName));
        QSize margins = delegate->getMargins();
        QSize _iconSize = iconSize();
        int iconXMargin = (visRect.width() - _iconSize.width()) / 2;
        int iconLeft = visRect.left() + iconXMargin;
        int iconTop = visRect.top() + margins.height();
        // the selection (hover) corner is a rectangle near the top left corner of
        // the icon and outside it as far as possible, so that its width and height
        // are 1/3 of the icon size >= 48 px (see FolderItemDelegate::paint)
        if(isCursorPos && _iconSize.width() >= 48
           && (selectionMode() == QAbstractItemView::ExtendedSelection
               || selectionMode() == QAbstractItemView::MultiSelection)) {
            int s = _iconSize.width() / 3;
            int icnLeft = std::max(visRect.left(), iconLeft - s);
            int icnTop = std::max(visRect.top(), iconTop - s);
            if(point.x() >= icnLeft &&  point.x() <= icnLeft + s
               && point.y() >= icnTop &&  point.y() <= icnTop + s) {
                cursorOnSelectionCorner_ = true;
                return index;
            }
        }
        if(point.y() < iconTop) { // above icon
            return QModelIndex();
        }
        else if(point.y() < visRect.top() + margins.height() + _iconSize.height()) { // on the icon area
            if(point.x() < iconLeft || point.x() > (visRect.right() + 1 - iconXMargin)) {
                // to the left or right of the icon
                return QModelIndex();
            }
        }
        else {
            QSize _textSize = delegate->iconViewTextSize(index);
            int textHMargin = (visRect.width() - _textSize.width()) / 2;
            if(point.y() > visRect.top() + margins.height() + _iconSize.height() + _textSize.height() // below text
               // on the text area but to the left or right of the text
               || point.x() < visRect.left() + textHMargin || point.x() > visRect.right() + 1 - textHMargin) {
                return QModelIndex();
            }
        }
        // qDebug() << "visualRect: " << visRect << "point:" << point;
    }
    return index;
}


// NOTE:
// QListView has a problem which I consider a bug or a design flaw.
// When you set movement property to Static, theoretically the icons
// should not be movable. However, if you turned on icon mode,
// the icons becomes freely movable despite the value of movement is Static.
// To overcome this bug, we override all drag handling methods, and
// call QAbstractItemView directly, bypassing QListView.
// In this way, we can workaround the buggy behavior.
// The drag handlers of QListView basically does the same things
// as its parent QAbstractItemView, but it also stores the currently
// dragged item and paint them in the view as needed.
// TODO: I really should file a bug report to Qt developers.

void FolderViewListView::dragEnterEvent(QDragEnterEvent* event) {
    QAbstractItemView::dragEnterEvent(event);
    //qDebug("dragEnterEvent");
    //static_cast<FolderView*>(parent())->childDragEnterEvent(event);
}

void FolderViewListView::dragLeaveEvent(QDragLeaveEvent* e) {
    QAbstractItemView::dragLeaveEvent(e);
    static_cast<FolderView*>(parent())->childDragLeaveEvent(e);
}

void FolderViewListView::dragMoveEvent(QDragMoveEvent* e) {
    QAbstractItemView::dragMoveEvent(e);
    static_cast<FolderView*>(parent())->childDragMoveEvent(e);
}

void FolderViewListView::dropEvent(QDropEvent* e) {
    static_cast<FolderView*>(parent())->childDropEvent(e);
    QAbstractItemView::dropEvent(e);
}

void FolderViewListView::mouseReleaseEvent(QMouseEvent* event) {
    // NOTE: With mouseReleaseEvent, event->buttons() excludes the button that caused the event
    // and so, it should not be used here. Instead, event->button() is used.

    bool activationWasAllowed = activationAllowed_;
    if(!style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick, nullptr, this)
       || event->button() != Qt::LeftButton
       // no activation with mouse when the cursor is on the selection corner
       || cursorOnSelectionCorner_) {
        activationAllowed_ = false;
    }

    QListView::mouseReleaseEvent(event);

    activationAllowed_ = activationWasAllowed;
    if(event->button() == Qt::LeftButton) {
        mouseLeftPressed_ = false;
    }
}

void FolderViewListView::mouseDoubleClickEvent(QMouseEvent* event) {
    bool activationWasAllowed = activationAllowed_;
    if(style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick, nullptr, this)
       || event->button() != Qt::LeftButton
       // no activation with mouse when the cursor is on the selection corner
       || cursorOnSelectionCorner_) {
        activationAllowed_ = false;
    }

    QListView::mouseDoubleClickEvent(event);

    activationAllowed_ = activationWasAllowed;
}

QModelIndex FolderViewListView::moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) {
    QAbstractItemModel* model_ = model();

    if(model_ && currentIndex().isValid()) {
        FolderView::ViewMode viewMode = static_cast<FolderView*>(parent())->viewMode();
        if((viewMode == FolderView::IconMode) || (viewMode == FolderView::ThumbnailMode)) {
            int next = (layoutDirection() == Qt::RightToLeft) ? - 1 : 1;

            if(cursorAction == QAbstractItemView::MoveRight) {
                return model_->index(currentIndex().row() + next, 0);
            }
            else if(cursorAction == QAbstractItemView::MoveLeft) {
                return model_->index(currentIndex().row() - next, 0);
            }
        }
    }

    return QListView::moveCursor(cursorAction, modifiers);
}

void FolderViewListView::currentChanged(const QModelIndex &current, const QModelIndex &previous) {
    QListView::currentChanged(current, previous);
    if(viewMode() == QListView::ListMode && current.isValid()) {
        // QListView has a bug that may reset the horizontal scrollbar
        // in the list mode when the current item changes. This is a workaround.
        QTimer::singleShot(0, this, [this] {
            if(currentIndex().isValid()) {
                scrollTo(currentIndex());
            }
        });
    }
}

void FolderViewListView::activation(const QModelIndex& index) {
    if(activationAllowed_) {
        Q_EMIT activatedFiltered(index);
    }
}

void FolderViewListView::selectAll() {
    // NOTE: By default QListView::selectAll() selects all columns in the model.
    // However, QListView only show the first column. Normal selection by mouse
    // can only select the first column of every row. I consider this discripancy yet
    // another design flaw of Qt. To make them consistent, we do it ourselves by only
    // selecting the first column of every row and do not select all columns as Qt does.
    // I'll report a Qt bug for this later.
    if(QAbstractItemModel* model_ = model()) {
        const QItemSelection sel{model_->index(0, 0), model_->index(model_->rowCount() - 1, 0)};
        selectionModel()->select(sel, QItemSelectionModel::Select);
    }
}

//-----------------------------------------------------------------------------

FolderViewTreeView::FolderViewTreeView(QWidget* parent):
    QTreeView(parent),
    doingLayout_(false),
    layoutTimer_(nullptr),
    activationAllowed_(true) {

    header()->setSectionResizeMode(QHeaderView::Interactive);
    header()->setStretchLastSection(true);

    // get the new width if the section is resized by user
    connect(header(), &QHeaderView::sectionResized, [this](int logicalIndex, int/* oldSize*/, int newSize) {
        if(doingLayout_ || customColumnWidths_.isEmpty()) {
            return;
        }
        int vIndx = header()->visualIndex(logicalIndex);
        if(vIndx >= 0 && vIndx < customColumnWidths_.size()) {
            customColumnWidths_[vIndx] = newSize;
            Q_EMIT columnResizedByUser(vIndx, newSize);
            queueLayoutColumns();
        }
    });

    // header context menu for configuring its resizing and hidden sections
    header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header(), &QWidget::customContextMenuRequested, this, &FolderViewTreeView::headerContextMenu);

    setIndentation(0);
    // the default true value may cause a crash on entering a folder by double clicking (a Qt bug?)
    setExpandsOnDoubleClick(false);

    connect(this, &QTreeView::activated, this, &FolderViewTreeView::activation);
    // don't open editor on double clicking
    setEditTriggers(QAbstractItemView::NoEditTriggers);
}

FolderViewTreeView::~FolderViewTreeView() {
    if(layoutTimer_) {
        delete layoutTimer_;
    }
}

void FolderViewTreeView::setCustomColumnWidths(const QList<int> &widths) {
    // enables cutomizable widths if "widths" is not empty; otherwise, enables auto-resizing
    if(customColumnWidths_ == widths) {
        return;
    }
    customColumnWidths_.clear();
    customColumnWidths_ = widths;
    header()->setStretchLastSection(widths.isEmpty());
    queueLayoutColumns();
    if(widths.isEmpty()) {
        Q_EMIT autoResizeEnabled();
    }
}

void FolderViewTreeView::setHiddenColumns(const QSet<int> &columns) {
    if(hiddenColumns_ == columns) {
        return;
    }
    hiddenColumns_.clear();
    hiddenColumns_ = columns;
    queueLayoutColumns();
}

void FolderViewTreeView::headerContextMenu(const QPoint &p) {
    QMenu menu(header()); // a parent is needed under Wayland for correct positioning
    QAction *action = menu.addAction (tr("Auto-resize columns"));
    action->setCheckable(true);
    action->setChecked(customColumnWidths_.isEmpty());
    connect(action, &QAction::triggered, action, [this] (bool checked) {
        QList<int> widths;
        if(!checked) {
            for(int column = 0; column < FolderModel::NumOfColumns; ++column) {
                widths << 0;
            }
            // one signal is enough to make a raw FolderView::customColumnWidths_
            Q_EMIT columnResizedByUser(0, 0);
        }
        setCustomColumnWidths(widths);
    });
    if(model()) {
        menu.addSeparator();
        QWidgetAction *labelAction = new QWidgetAction(&menu);
        QLabel *label = new QLabel(QStringLiteral("<center><b>") + tr("Visible Columns") + QStringLiteral("</b></center>"));
        labelAction->setDefaultWidget(label);
        menu.addAction (labelAction);

        int filenameColumn = header()->visualIndex(FolderModel::ColumnFileName);
        int dTimeColumn = header()->visualIndex(FolderModel::ColumnFileDTime);
        bool isTrash = false;
        if(ProxyFolderModel* proxyModel = qobject_cast<ProxyFolderModel*>(model())) {
            if(auto model = static_cast<FolderModel*>(proxyModel->sourceModel())) {
                if(model->path() && strcmp(model->path().toString().get(), "trash:///") == 0) {
                    isTrash = true;
                }
            }
        }
        int numCols = header()->count();
        for(int column = 0; column < numCols; ++column) {
            int columnId = header()->logicalIndex(column);
            if(!isTrash && columnId == dTimeColumn) {
                // no action for the deletion time column if this isn't trash
                continue;
            }
            if(columnId >= 0 && columnId < FolderModel::NumOfColumns) {
                action = menu.addAction (model()->headerData(columnId, Qt::Horizontal, Qt::DisplayRole).toString());
                action->setCheckable(true);
                if(columnId == filenameColumn) { // never hide the name column
                    action->setChecked(true);
                    action->setDisabled(true);
                }
                else {
                    action->setChecked(!header()->isSectionHidden(columnId));
                    connect(action, &QAction::triggered, action, [this, column] (bool checked) {
                        if(checked) {
                            hiddenColumns_.remove(column);
                        }
                        else {
                            hiddenColumns_ << column;
                        }
                        Q_EMIT columnHiddenByUser(column, !checked);
                        queueLayoutColumns();
                    });
                }
            }
        }
    }
    menu.exec(header()->mapToGlobal(p));
}

void FolderViewTreeView::setModel(QAbstractItemModel* model) {
    QTreeView::setModel(model);
    layoutColumns();
    if(ProxyFolderModel* proxyModel = qobject_cast<ProxyFolderModel*>(model)) {
        connect(proxyModel, &ProxyFolderModel::sortFilterChanged, this, &FolderViewTreeView::onSortFilterChanged,
                Qt::UniqueConnection);
        onSortFilterChanged();
    }
}

void FolderViewTreeView::paintEvent(QPaintEvent * event) {
    QTreeView::paintEvent(event);
    if(rubberBandRect_.isValid()) { // draw rubberband
        QPainter p(viewport());
        QStyleOptionRubberBand opt;
        opt.initFrom(this);
        opt.shape = QRubberBand::Rectangle;
        opt.opaque = false;
        QRect r = rubberBandRect_.adjusted(-horizontalOffset(), -verticalOffset(),
                                            -horizontalOffset(), -verticalOffset())
                    .intersected(viewport()->rect()
                    .adjusted(-16, -16, 16, 16));
        opt.rect = r;
        style()->drawControl(QStyle::CE_RubberBand, &opt, &p);
    }
}

void FolderViewTreeView::mousePressEvent(QMouseEvent* event) {
    if(event->buttons() == Qt::LeftButton) { // see FolderViewTreeView::mouseMoveEvent
        globalItemPressPoint_ = event->globalPosition().toPoint();
    }
    if(selectionMode() == QAbstractItemView::ExtendedSelection) {
        // remember mouse press position and determine whether selections should be kept
        // or removed later, when the cursor moves
        QAbstractItemView::mousePressEvent(event);
        mousePressPoint_ = event->position().toPoint() + QPoint(horizontalOffset(), verticalOffset());
    }
    else {
        QTreeView::mousePressEvent(event);
    }

    static_cast<FolderView*>(parent())->childMousePressEvent(event);
}

void FolderViewTreeView::mouseMoveEvent(QMouseEvent* event) {
    // NOTE: Filter the BACK & FORWARD buttons to not Drag & Drop with them.
    // (by default Qt views drag with any button)
    if(event->buttons() == Qt::NoButton || (event->buttons() & ~(Qt::BackButton | Qt::ForwardButton))) {
        // handle rubberband
        if(selectionMode() == QAbstractItemView::ExtendedSelection
            && (event->buttons() & Qt::LeftButton)
            && (rubberBandRect_.isValid()
                || !indexAt(mousePressPoint_ - QPoint(horizontalOffset(), verticalOffset())).isValid()
                // also, start the rubberband selection from any column other than the name column
                || columnAt(mousePressPoint_.x() - horizontalOffset()) != FolderModel::ColumnFileName)) {
            QAbstractItemView::mouseMoveEvent(event);

            // set rubberband rectangle
            QRect rect(mousePressPoint_, event->position().toPoint() + QPoint(horizontalOffset(), verticalOffset()));
            rect = rect.normalized();
            QRect r = rect.united(rubberBandRect_);
            viewport()->update(r.adjusted(-horizontalOffset(), -verticalOffset(),
                                            -horizontalOffset(), -verticalOffset()));
            rubberBandRect_ = rect;

            // set state and selection
            setState(QAbstractItemView::DragSelectingState);
            Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
            QItemSelectionModel::SelectionFlags command;
            if(modifiers & Qt::ControlModifier) {
                command = QItemSelectionModel::ToggleCurrent;
            }
            else if(modifiers & Qt::ShiftModifier) {
                command = QItemSelectionModel::SelectCurrent;
            }
            else {
                command = QItemSelectionModel::Clear|QItemSelectionModel::SelectCurrent;
            }
            command |= QItemSelectionModel::Rows;
            QRect selectionRect = QRect(rubberBandRect_.topLeft(), rubberBandRect_.bottomRight());
            setSelection(selectionRect, command);
        }
        else {
            // don't start drag if the cursor isn't moved since pressing left mouse button on an item
            // because the user may want to scroll the view with mouse wheel before dragging
            if(!(event->buttons() == Qt::LeftButton
                 && (globalItemPressPoint_ - event->globalPosition().toPoint()).manhattanLength() <= QApplication::startDragDistance())) {
                QTreeView::mouseMoveEvent(event);
            }
        }
    }
}

void FolderViewTreeView::setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags command) {
    if(selectionMode() == QAbstractItemView::ExtendedSelection
       && model() && state() == QAbstractItemView::DragSelectingState
       && !rubberBandRect_.isNull()) { // rubberband selection
        QRect r = rubberBandRect_.adjusted(-horizontalOffset(), -verticalOffset(),
                                            -horizontalOffset(), -verticalOffset());
        bool rtl(layoutDirection() == Qt::RightToLeft);
        if (rtl) {
            r.setRight(std::min(viewport()->contentsRect().right(), r.right()));
        }
        else {
            r.setLeft(std::max(viewport()->contentsRect().left(), r.left()));
        }
        r.setTop(std::max(-verticalOffset(), r.top()));
        QModelIndex top = indexAt(rtl ? r.topRight() : r.topLeft());
        QItemSelection selection;
        if(top.isValid()) {
            top = top.sibling(top.row(), 0);
             if(top.isValid()) {
                QModelIndex bottom = indexAt(rtl ? r.bottomRight() : r.bottomLeft());
                if(!bottom.isValid()) {
                    bottom = model()->index(model()->rowCount() - 1, 0);
                }
                if(bottom.isValid()) {
                    selection = QItemSelection(top, bottom);
                }
             }
        }
        selectionModel()->select(selection, command | QItemSelectionModel::Rows);
    }
    else {
        QTreeView::setSelection(rect, command);
    }
}

void FolderViewTreeView::dragEnterEvent(QDragEnterEvent* event) {
    QTreeView::dragEnterEvent(event);
    //static_cast<FolderView*>(parent())->childDragEnterEvent(event);
}

void FolderViewTreeView::dragLeaveEvent(QDragLeaveEvent* e) {
    QTreeView::dragLeaveEvent(e);
    static_cast<FolderView*>(parent())->childDragLeaveEvent(e);
}

void FolderViewTreeView::dragMoveEvent(QDragMoveEvent* e) {
    QTreeView::dragMoveEvent(e);
    static_cast<FolderView*>(parent())->childDragMoveEvent(e);
}

void FolderViewTreeView::dropEvent(QDropEvent* e) {
    static_cast<FolderView*>(parent())->childDropEvent(e);
    QTreeView::dropEvent(e);
}

// the default list mode of QListView handles column widths
// very badly (worse than gtk+) and it's not very flexible.
// so, let's handle column widths ourselves.
void FolderViewTreeView::layoutColumns() {
    // qDebug("layoutColumns");
    if(!model()) {
        return;
    }
    doingLayout_ = true;
    QHeaderView* headerView = header();
    // the width that's available for showing the columns.
    int availWidth = viewport()->contentsRect().width();

    // get the width that every column want
    int numCols = headerView->count();
    if(numCols > 0) {
        int desiredWidth = 0;
        QList<int> widths; // to store the widths that the columns need
        QStyleOptionHeader opt;
        opt.initFrom(headerView);
        opt.fontMetrics = QFontMetrics(font());
        if (headerView->isSortIndicatorShown()) {
            opt.sortIndicator = QStyleOptionHeader::SortDown;
        }
        QAbstractItemModel* model_ = model();
        int filenameColumn = headerView->visualIndex(FolderModel::ColumnFileName);
        int dTimeColumn = header()->visualIndex(FolderModel::ColumnFileDTime);
        bool isTrash = false;
        if(ProxyFolderModel* proxyModel = qobject_cast<ProxyFolderModel*>(model())) {
            if(auto model = static_cast<FolderModel*>(proxyModel->sourceModel())) {
                if(model->path() && strcmp(model->path().toString().get(), "trash:///") == 0) {
                    isTrash = true;
                }
            }
        }
        for(int column = 0; column < numCols; ++column) {
            int columnId = headerView->logicalIndex(column);

            if(!isTrash && columnId == dTimeColumn) {
                // hide the deletion time column if this isn't trash
                headerView->setSectionHidden(columnId, true);
                widths.append(headerView->minimumSectionSize());
                continue;
            }

            // handle hidden columns
            bool wasHidden = false;
            if(headerView->isSectionHidden(columnId)) {
                if(!hiddenColumns_.contains(columnId)) {
                    headerView->setSectionHidden(columnId, false);
                    wasHidden = true;
                }
                else {
                    widths.append(headerView->minimumSectionSize());
                    continue;
                }
            }
            else if(hiddenColumns_.contains(columnId)
                    && columnId != filenameColumn) { // never hide the name column
                headerView->setSectionHidden(columnId, true);
                widths.append(headerView->minimumSectionSize());
                continue;
            }

            int w;
            if(customColumnWidths_.size() > column) {
                // see FolderView::setCustomColumnWidths for the meaning of custom width <= 0
                if(customColumnWidths_.at(column) > 0) {
                    w = std::max(customColumnWidths_.at(column), headerView->minimumSectionSize());
                }
                else {
                    if(wasHidden) {
                        // WARNING: When a section is shown in the interactive mode, Qt gives
                        // a huge width to it. As a workaround, the width is set to the minimum here.
                        customColumnWidths_[column] = w = headerView->minimumSectionSize();
                    }
                    else {
                        customColumnWidths_[column] = w = headerView->sectionSize(columnId);
                    }
                    Q_EMIT columnResizedByUser(column, customColumnWidths_.at(column));
                }
            }
            else {
                // get the size that the column needs
                if(model_) {
                    QVariant data = model_->headerData(columnId, Qt::Horizontal, Qt::DisplayRole);
                    if(data.isValid()) {
                        opt.text = data.isValid() ? data.toString() : QString();
                    }
                }
                opt.section = columnId;
                w = std::max(sizeHintForColumn(columnId),
                             style()->sizeFromContents(QStyle::CT_HeaderSection, &opt, QSize(),
                                                       headerView).width());
            }
            widths.append(w);
            // compute the total width needed
            desiredWidth += w;
        }

        if(customColumnWidths_.size() <= filenameColumn) { // practically means no custom width
            // if the total width we want exceeds the available space
            if(desiredWidth > availWidth) {
                // Compute the width available for the filename column
                int filenameAvailWidth = availWidth - desiredWidth + widths.at(filenameColumn);

                // Compute the minimum acceptable width for the filename column, showing
                // whole texts whose lengths are less than 30 times the average font width.
                int filenameMinWidth = std::min(iconSize().width()
                                                + 30 * opt.fontMetrics.averageCharWidth(),
                                                sizeHintForColumn(filenameColumn));

                if(filenameAvailWidth > filenameMinWidth) {
                    // Shrink the filename column to the available width
                    widths[filenameColumn] = filenameAvailWidth;
                }
                else {
                    // Set the filename column to its minimum width
                    widths[filenameColumn] = filenameMinWidth;
                }
            }
            else {
                // Fill the extra available space with the filename column
                widths[filenameColumn] += availWidth - desiredWidth;
            }
        }

        // really do the resizing for every column
        for(int column = 0; column < numCols; ++column) {
            headerView->resizeSection(headerView->logicalIndex(column), widths.at(column));
        }
    }
    doingLayout_ = false;

    if(layoutTimer_) {
        delete layoutTimer_;
        layoutTimer_ = nullptr;
    }
    setUpdatesEnabled(true);
}

void FolderViewTreeView::resizeEvent(QResizeEvent* event) {
    QAbstractItemView::resizeEvent(event);
    // prevent endless recursion.
    // When manually resizing columns, at the point where a horizontal scroll
    // bar has to be inserted or removed, the vertical size changes, a resize
    // event  occurs and the column headers are flickering badly if the column
    // layout is modified at this point. Therefore only layout the columns if
    // the horizontal size changes.
    if(!doingLayout_ && event->size().width() != event->oldSize().width()) {
        layoutColumns();    // layoutColumns() also triggers resizeEvent
    }
}

void FolderViewTreeView::rowsInserted(const QModelIndex& parent, int start, int end) {
    setUpdatesEnabled(false); // prevent header text flickering
    queueLayoutColumns();
    QTreeView::rowsInserted(parent, start, end);
}

void FolderViewTreeView::rowsAboutToBeRemoved(const QModelIndex& parent, int start, int end) {
    QTreeView::rowsAboutToBeRemoved(parent, start, end);
    queueLayoutColumns();
}

void FolderViewTreeView::dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles /*= QList<int>{}*/) {
    QTreeView::dataChanged(topLeft, bottomRight, roles);
    // FIXME: this will be very inefficient
    // queueLayoutColumns();
}

void FolderViewTreeView::reset() {
    // Sometimes when the content of the model is radically changed, Qt does reset()
    // on the model rather than doing large amount of insertion and deletion.
    // This is for performance reason so in this case rowsInserted() and rowsAboutToBeRemoved()
    // might not be called. Hence we also have to re-layout the columns when the model is reset.
    // This fixes bug #190
    // https://github.com/lxqt/pcmanfm-qt/issues/190
    setUpdatesEnabled(false); // prevent header text flickering
    queueLayoutColumns();
    QTreeView::reset();
}

void FolderViewTreeView::queueLayoutColumns() {
    // qDebug("queueLayoutColumns");
    if(!layoutTimer_) {
        layoutTimer_ = new QTimer();
        layoutTimer_->setSingleShot(true);
        layoutTimer_->setInterval(0);
        connect(layoutTimer_, &QTimer::timeout, this, &FolderViewTreeView::layoutColumns);
    }
    layoutTimer_->start();
}

void FolderViewTreeView::mouseReleaseEvent(QMouseEvent* event) {
    bool activationWasAllowed = activationAllowed_;
    if(!style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick, nullptr, this)
       || event->button() != Qt::LeftButton
       // also, prevent activation if rubberband selection is greater than dragging distance
       || rubberBandRect_.width() + rubberBandRect_.height() > QApplication::startDragDistance()) {
        activationAllowed_ = false;
    }

    if(selectionMode() == QAbstractItemView::ExtendedSelection) {
        QAbstractItemView::mouseReleaseEvent(event);
        viewport()->update(rubberBandRect_.adjusted(-horizontalOffset(), -verticalOffset(),
                                                    -horizontalOffset(), -verticalOffset()));
        rubberBandRect_ = QRect();
    }
    else {
        QTreeView::mouseReleaseEvent(event);
    }

    activationAllowed_ = activationWasAllowed;

}

void FolderViewTreeView::mouseDoubleClickEvent(QMouseEvent* event) {
    bool activationWasAllowed = activationAllowed_;
    if((style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick, nullptr, this)) || (event->button() != Qt::LeftButton)) {
        activationAllowed_ = false;
    }

    QTreeView::mouseDoubleClickEvent(event);

    activationAllowed_ = activationWasAllowed;
}

void FolderViewTreeView::activation(const QModelIndex& index) {
    if(activationAllowed_) {
        Q_EMIT activatedFiltered(index);
    }
}

void FolderViewTreeView::onSortFilterChanged() {
    if(QSortFilterProxyModel* proxyModel = qobject_cast<QSortFilterProxyModel*>(model())) {
        header()->setSortIndicatorShown(true);
        header()->setSortIndicator(proxyModel->sortColumn(), proxyModel->sortOrder());
        if(!isSortingEnabled()) {
            setSortingEnabled(true);
        }
    }
}


//-----------------------------------------------------------------------------

FolderView::FolderView(FolderView::ViewMode _mode, QWidget *parent):
    QWidget(parent),
    view(nullptr),
    model_(nullptr),
    mode((ViewMode)0),
    fileLauncher_(nullptr),
    autoSelectionDelay_(600),
    autoSelectionTimer_(nullptr),
    selChangedTimer_(nullptr),
    itemDelegateMargins_(QSize(3, 3)),
    shadowHidden_(false),
    scrollPerPixel_(true),
    ctrlRightClick_(false),
    smoothScrollTimer_(nullptr) {

    iconSize_[IconMode - FirstViewMode] = QSize(48, 48);
    iconSize_[CompactMode - FirstViewMode] = QSize(24, 24);
    iconSize_[ThumbnailMode - FirstViewMode] = QSize(128, 128);
    iconSize_[DetailedListMode - FirstViewMode] = QSize(24, 24);

    QVBoxLayout* layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);

    setViewMode(_mode);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    connect(this, &FolderView::clicked, this, &FolderView::onFileClicked);
}

FolderView::~FolderView() {
    if(smoothScrollTimer_) {
        disconnect(smoothScrollTimer_, &QTimer::timeout, this, &FolderView::scrollSmoothly);
        smoothScrollTimer_->stop();
        delete smoothScrollTimer_;
    }
}

void FolderView::setCustomColumnWidths(const QList<int> &widths) {
    customColumnWidths_.clear();
    customColumnWidths_ = widths;
    // Complete the widths list with zeros if needed. A value of <= 0 means that
    // the initial custom width of the column should be set to its current width.
    if(!customColumnWidths_.isEmpty()) {
        for(int i = customColumnWidths_.size(); i < FolderModel::NumOfColumns; ++i) {
            customColumnWidths_ << 0;
        }
    }
    // resize header sections to custom widths if the tree view exists
    if(mode == DetailedListMode) {
        if(FolderViewTreeView* treeView = static_cast<FolderViewTreeView*>(view)) {
            treeView->setCustomColumnWidths(customColumnWidths_);
        }
    }
}

void FolderView::setHiddenColumns(const QList<int> &columns) {
    hiddenColumns_.clear();
    hiddenColumns_ = QSet<int>(columns.begin(), columns.end());
    if(mode == DetailedListMode) {
        if(FolderViewTreeView* treeView = static_cast<FolderViewTreeView*>(view)) {
            treeView->setHiddenColumns(hiddenColumns_);
        }
    }
}

void FolderView::setScrollPerPixel(bool perPixel) {
    if(scrollPerPixel_ == perPixel) {
        return;
    }
    scrollPerPixel_ = perPixel;
    if(!scrollPerPixel_ && smoothScrollTimer_ != nullptr) {
        disconnect(smoothScrollTimer_, &QTimer::timeout, this, &FolderView::scrollSmoothly);
        smoothScrollTimer_->stop();
        delete smoothScrollTimer_;
        smoothScrollTimer_ = nullptr;
        queuedScrollSteps_.clear(); // also forget the remaining steps
    }

    // icon and thumbnail modes scroll per pixel by default
    if(mode == DetailedListMode) {
        if(FolderViewTreeView* treeView = static_cast<FolderViewTreeView*>(view)) {
            treeView->setVerticalScrollMode(scrollPerPixel_ ? QAbstractItemView::ScrollPerPixel
                                                            : QAbstractItemView::ScrollPerItem);
        }
    }
    else if(mode == CompactMode) {
        if(FolderViewListView* listView = static_cast<FolderViewListView*>(view)) {
            listView->setHorizontalScrollMode(scrollPerPixel_ ? QAbstractItemView::ScrollPerPixel
                                                              : QAbstractItemView::ScrollPerItem);
        }
    }
}

void FolderView::onItemActivated(QModelIndex index) {
    if(QApplication::keyboardModifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        return;
    }
    if(QItemSelectionModel* selModel = selectionModel()) {
        QVariant data;
        if(index.isValid() && selModel->isSelected(index)) { // activate index only if it is selected
            if(index.model()) {
                data = index.model()->data(index, FolderModel::FileInfoRole);
            }
        }
        else { // if index is not valid or selected, activate the first selected index
            QModelIndexList selIndexes = mode == DetailedListMode ? selectedRows() : selectedIndexes();
            if(!selIndexes.isEmpty()) {
                QModelIndex first = selIndexes.first();
                if(first.model()) {
                    data = first.model()->data(first, FolderModel::FileInfoRole);
                }
            }
        }
        if(data.isValid()) {
            auto info = data.value<std::shared_ptr<const Fm::FileInfo>>();
            if(info) {
                Q_EMIT clicked(ActivatedClick, info);
            }
        }
    }
}

void FolderView::onSelChangedTimeout() {
    selChangedTimer_->deleteLater();
    selChangedTimer_ = nullptr;
    // qDebug()<<"selected:" << nSel;
    Q_EMIT selChanged();
}

void FolderView::onSelectionChanged(const QItemSelection& /*selected*/, const QItemSelection& /*deselected*/) {
    // It's possible that the selected items change too often and this slot gets called for thousands of times.
    // For example, when you select thousands of files and delete them, we will get one selectionChanged() event
    // for every deleted file. So, we use a timer to delay the handling to avoid too frequent updates of the UI.
    if(!selChangedTimer_) {
        selChangedTimer_ = new QTimer(this);
        selChangedTimer_->setSingleShot(true);
        connect(selChangedTimer_, &QTimer::timeout, this, &FolderView::onSelChangedTimeout);
        selChangedTimer_->start(200);
    }
}

void FolderView::onClosingEditor(QWidget* editor, QAbstractItemDelegate::EndEditHint hint) {
    if (hint != QAbstractItemDelegate::NoHint) {
        // we set the hint to NoHint in FolderItemDelegate::eventFilter()
        return;
    }
    QString newName;
    if (qobject_cast<QTextEdit*>(editor)) { // icon and thumbnail view
        newName = qobject_cast<QTextEdit*>(editor)->toPlainText();
    }
    else if (qobject_cast<QLineEdit*>(editor)) { // compact view
        newName = qobject_cast<QLineEdit*>(editor)->text();
    }
    if (newName.isEmpty()) {
        return;
    }
    // the editor will be deleted by QAbstractItemDelegate::destroyEditor() when no longer needed

    QModelIndex index = view->selectionModel()->currentIndex();
    if(index.isValid() && index.model()) {
        QVariant data = index.model()->data(index, FolderModel::FileInfoRole);
        auto info = data.value<std::shared_ptr<const Fm::FileInfo>>();
        if (info) {
            // NOTE: "Edit name" is used to handle invalid filename encoding.
            auto oldName = QString::fromUtf8(g_file_info_get_edit_name(info->gFileInfo().get()));
            if(oldName.isEmpty()) {
                oldName = QString::fromStdString(info->name());
            }
            if(newName == oldName) {
                return;
            }
            QWidget* parent = window();
            if (window() == this) { // supposedly desktop, in case it uses this
                parent = nullptr;
            }
            if(changeFileName(info->path(), newName, parent)) {
                Q_EMIT inlineRenamed(oldName, newName);
            }
        }
    }
}

void FolderView::setViewMode(ViewMode _mode) {
    if(_mode == mode) { // if it's the same more, ignore
        return;
    }
    // FIXME: retain old selection

    // since only detailed list mode uses QTreeView, and others
    // all use QListView, it's wise to preserve QListView when possible.
    bool recreateView = false;
    if(view && (mode == DetailedListMode || _mode == DetailedListMode)) {
        delete view; // FIXME: no virtual dtor?
        view = nullptr;
        recreateView = true;
    }
    mode = _mode;
    QSize iconSize = iconSize_[mode - FirstViewMode];

    FolderItemDelegate* delegate = nullptr;
    if(mode == DetailedListMode) {
        FolderViewTreeView* treeView = new FolderViewTreeView(this);
        if(scrollPerPixel_) {
            treeView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        }
        treeView->setCustomColumnWidths(customColumnWidths_);
        treeView->setHiddenColumns(hiddenColumns_);
        treeView->setAlternatingRowColors(true);
        connect(treeView, &FolderViewTreeView::activatedFiltered, this, &FolderView::onItemActivated);
        // update the list of custom widhts when the user changes it
        connect(treeView, &FolderViewTreeView::columnResizedByUser, [this](int visualIndex, int newWidth) {
            if(visualIndex >= 0) {
                if(visualIndex < customColumnWidths_.size()){
                    customColumnWidths_[visualIndex] = newWidth;
                }
                else {
                    customColumnWidths_ << newWidth;
                }
                // complete the widths list with zeros if needed
                for(int i = customColumnWidths_.size(); i < FolderModel::NumOfColumns; ++i) {
                    customColumnWidths_ << 0;
                }
                Q_EMIT columnResizedByUser();
            }
        });
        connect(treeView, &FolderViewTreeView::autoResizeEnabled, [this]() {
            customColumnWidths_.clear();
            Q_EMIT columnResizedByUser();
        });
        // update the list of hidden columns when the user changes it
        connect(treeView, &FolderViewTreeView::columnHiddenByUser, [this](int visibleIndex, bool hidden) {
            if(hidden) {
                hiddenColumns_ << visibleIndex;
            }
            else {
                hiddenColumns_.remove(visibleIndex);
            }
            Q_EMIT columnHiddenByUser();
        });
        setFocusProxy(treeView);

        view = treeView;
        treeView->setItemsExpandable(false);
        treeView->setRootIsDecorated(false);
        treeView->setAllColumnsShowFocus(false);

        // set our own custom delegate
        delegate = new FolderItemDelegate(treeView);
        delegate->setShadowHidden(shadowHidden_);
        treeView->setItemDelegateForColumn(FolderModel::ColumnFileName, delegate);
    }
    else {
        FolderViewListView* listView;
        if(view) {
            listView = static_cast<FolderViewListView*>(view);
        }
        else {
            listView = new FolderViewListView(this);
            connect(listView, &FolderViewListView::activatedFiltered, this, &FolderView::onItemActivated);
            view = listView;
        }
        if(scrollPerPixel_ && mode == CompactMode) {
            listView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        }
        setFocusProxy(listView);

        // set our own custom delegate
        delegate = new FolderItemDelegate(listView);
        delegate->setShadowHidden(shadowHidden_);
        listView->setItemDelegateForColumn(FolderModel::ColumnFileName, delegate);
        listView->setResizeMode(QListView::Adjust);
        listView->setWrapping(true);
        switch(mode) {
        case IconMode: {
            listView->setViewMode(QListView::IconMode);
            listView->setWordWrap(true);
            listView->setFlow(QListView::LeftToRight);
            break;
        }
        case CompactMode: {
            listView->setViewMode(QListView::ListMode);
            listView->setWordWrap(false);
            listView->setFlow(QListView::QListView::TopToBottom);
            break;
        }
        case ThumbnailMode: {
            listView->setViewMode(QListView::IconMode);
            listView->setWordWrap(true);
            listView->setFlow(QListView::LeftToRight);
            break;
        }
        default:
            ;
        }
        updateGridSize();
    }
    if(view) {
        // we have to install the event filter on the viewport instead of the view itself.
        view->viewport()->installEventFilter(this);
        // we want the QEvent::HoverMove event for single click + auto-selection support
        view->viewport()->setAttribute(Qt::WA_Hover, true);
        view->setContextMenuPolicy(Qt::NoContextMenu); // defer the context menu handling to parent widgets
        view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        view->setIconSize(iconSize);

        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        layout()->addWidget(view);

        // enable dnd (the drop indicator is set at "FolderView::childDragMoveEvent()")
        view->setDragEnabled(true);
        view->setAcceptDrops(true);
        view->setDragDropMode(QAbstractItemView::DragDrop);

        // inline renaming
        connect(delegate, &QAbstractItemDelegate::closeEditor, this, &FolderView::onClosingEditor);

        if(model_) {
            // FIXME: preserve selections
            model_->setThumbnailSize(iconSize.width());
            view->setModel(model_);
            if(recreateView) {
                connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FolderView::onSelectionChanged);
            }
        }
    }
}

// set proper grid size for the QListView based on current view mode, icon size, and font size.
void FolderView::updateGridSize() {
    if(mode == DetailedListMode || !view) {
        return;
    }
    FolderViewListView* listView = static_cast<FolderViewListView*>(view);
    QSize icon = iconSize(mode); // size of the icon
    QFontMetrics fm = fontMetrics(); // size of current font
    QSize grid; // the final grid size
    switch(mode) {
    case IconMode:
    case ThumbnailMode: {
        // NOTE by PCMan about finding the optimal text label size:
        // The average filename length on my root filesystem is roughly 18-20 chars.
        // So, a reasonable size for the text label is about 10 chars each line since string of this length
        // can be shown in two lines. If you consider word wrap, then the result is around 10 chars per word.
        // In average, 10 char per line should be enough to display a "word" in the filename without breaking.
        // The values can be estimated with this command:
        // > find / | xargs  basename -a | sed -e s'/[_-]/ /g' | wc -mcw
        // However, this average only applies to English. For some Asian characters, such as Chinese chars,
        // each char actually takes doubled space. To be safe, we use 13 chars per line x average char width
        // to get a nearly optimal width for the text label. As most of the filenames have less than 40 chars
        // 13 chars x 3 lines should be enough to show the full filenames for most files.
        int textWidth = fm.averageCharWidth() * 13;
        int textHeight = fm.lineSpacing() * 3;
        grid.setWidth(std::max(icon.width(), textWidth) + 4); // a margin of 2 px for selection rects
        grid.setHeight(icon.height() + textHeight + 4); // a margin of 2 px for selection rects
        // grow to include margins
        grid += 2*itemDelegateMargins_;
        // let horizontal and vertical spacings be set only by itemDelegateMargins_
        listView->setSpacing(0);

        break;
    }
    default:
        // FIXME: set proper item size
        listView->setSpacing(2);
        ; // do not use grid size
    }

    FolderItemDelegate* delegate = static_cast<FolderItemDelegate*>(listView->itemDelegateForColumn(FolderModel::ColumnFileName));
    delegate->setItemSize(grid);
    delegate->setIconSize(icon);
    delegate->setMargins(itemDelegateMargins_);
}

void FolderView::setIconSize(ViewMode mode, QSize size) {
    Q_ASSERT(mode >= FirstViewMode && mode <= LastViewMode);
    iconSize_[mode - FirstViewMode] = size;
    if(viewMode() == mode) {
        view->setIconSize(size);
        if(model_) {
            model_->setThumbnailSize(size.width());
        }
        updateGridSize();
    }
}

QSize FolderView::iconSize(ViewMode mode) const {
    Q_ASSERT(mode >= FirstViewMode && mode <= LastViewMode);
    return iconSize_[mode - FirstViewMode];
}

void FolderView::setMargins(QSize size) {
    if(itemDelegateMargins_ != size.expandedTo(QSize(0, 0))) {
        itemDelegateMargins_ = size.expandedTo(QSize(0, 0));
        updateGridSize();
    }
}

void FolderView::setShadowHidden(bool shadowHidden) {
    if(view && shadowHidden != shadowHidden_) {
        shadowHidden_ = shadowHidden;
        FolderItemDelegate* delegate = nullptr;
        if(mode == DetailedListMode) {
            FolderViewTreeView* treeView = static_cast<FolderViewTreeView*>(view);
            delegate = static_cast<FolderItemDelegate*>(treeView->itemDelegateForColumn(FolderModel::ColumnFileName));
        }
        else {
            FolderViewListView* listView = static_cast<FolderViewListView*>(view);
            delegate = static_cast<FolderItemDelegate*>(listView->itemDelegateForColumn(FolderModel::ColumnFileName));
        }
        if(delegate) {
            delegate->setShadowHidden(shadowHidden);
        }
    }
}

void FolderView::setCtrlRightClick(bool ctrlRightClick) {
    ctrlRightClick_ = ctrlRightClick;
}

FolderView::ViewMode FolderView::viewMode() const {
    return mode;
}

void FolderView::setAutoSelectionDelay(int delay) {
    autoSelectionDelay_ = delay;
    if(autoSelectionDelay_ <= 0 && autoSelectionTimer_) {
        autoSelectionTimer_->stop();
        delete autoSelectionTimer_;
        autoSelectionTimer_ = nullptr;
    }
}

QAbstractItemView* FolderView::childView() const {
    return view;
}

ProxyFolderModel* FolderView::model() const {
    return model_;
}

void FolderView::setModel(ProxyFolderModel* model) {
    if(view) {
        view->setModel(model);
        QSize iconSize = iconSize_[mode - FirstViewMode];
        model->setThumbnailSize(iconSize.width());
        if(view->selectionModel()) {
            connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FolderView::onSelectionChanged);
        }
    }
    if(model_) {
        delete model_;
    }
    model_ = model;
}

bool FolderView::event(QEvent* event) {
    switch(event->type()) {
    case QEvent::StyleChange:
        break;
    case QEvent::FontChange:
        updateGridSize();
        break;
    case QEvent::KeyPress:
        // Pressing Enter activates only the current index. With no current index,
        // we activate the first selected index on pressing Enter (see onItemActivated).
        if(view && !view->selectionModel()->currentIndex().isValid()) {
            int k = static_cast<QKeyEvent*>(event)->key();
            if(k == Qt::Key_Return || k == Qt::Key_Enter) {
                onItemActivated(QModelIndex());
            }
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void FolderView::contextMenuEvent(QContextMenuEvent* event) {
    QWidget::contextMenuEvent(event);
    QPoint pos = event->pos();
    QPoint view_pos = view->mapFromParent(pos);
    QPoint viewport_pos = view->viewport()->mapFromParent(view_pos);
    emitClickedAt(ContextMenuClick, viewport_pos);
}

void FolderView::childMousePressEvent(QMouseEvent* event) {
    // called from mousePressEvent() of child view
    Qt::MouseButton button = event->button();
    if(button == Qt::MiddleButton) {
        emitClickedAt(MiddleClick, event->pos());
    }
    else if(button == Qt::BackButton) {
        Q_EMIT clickedBack();
    }
    else if(button == Qt::ForwardButton) {
        Q_EMIT clickedForward();
    }
}

void FolderView::emitClickedAt(ClickType type, const QPoint& pos) {
    // indexAt() needs a point in "viewport" coordinates.
    QModelIndex index = view->indexAt(pos);
    if(index.isValid()
       && (!ctrlRightClick_ || QApplication::keyboardModifiers() != Qt::ControlModifier)) {
        if(mode == DetailedListMode && type == ContextMenuClick) {
            // WARNING: Qt has a bug which reports a wrong cursor position in QContextMenuEvent
            // with the detailed list mode when the context menu is invoked by the Menu key.
            // Since the bug has nasty effects for us, we include a safe workaround here.
            if(QItemSelectionModel* selModel = selectionModel()) {
                if(selModel->isSelected(selModel->currentIndex())) {
                    index = selModel->currentIndex();
                }
            }
        }
        QVariant data = index.data(FolderModel::FileInfoRole);
        auto info = data.value<std::shared_ptr<const Fm::FileInfo>>();
        Q_EMIT clicked(type, info);
    }
    else {
        // FIXME: should we show popup menu for the selected files instead
        // if there are selected files?
        if(type == ContextMenuClick) {
            // clear current selection if clicked outside selected files
            view->clearSelection();
            Q_EMIT clicked(type, nullptr);
        }
    }
}

QModelIndexList FolderView::selectedRows(int column) const {
    QItemSelectionModel* selModel = selectionModel();
    if(selModel) {
        return selModel->selectedRows(column);
    }
    return QModelIndexList();
}

// This returns all selected "cells", which means all cells of the same row are returned.
QModelIndexList FolderView::selectedIndexes() const {
    QItemSelectionModel* selModel = selectionModel();
    if(selModel) {
        return selModel->selectedIndexes();
    }
    return QModelIndexList();
}

QItemSelectionModel* FolderView::selectionModel() const {
    return view ? view->selectionModel() : nullptr;
}

Fm::FilePathList FolderView::selectedFilePaths() const {
    if(model_) {
        QModelIndexList selIndexes = mode == DetailedListMode ? selectedRows() : selectedIndexes();
        if(!selIndexes.isEmpty()) {
            Fm::FilePathList paths;
            QModelIndexList::const_iterator it;
            for(it = selIndexes.constBegin(); it != selIndexes.constEnd(); ++it) {
                auto file = model_->fileInfoFromIndex(*it);
                paths.push_back(file->path());
            }
            return paths;
        }
    }
    return Fm::FilePathList();
}

bool FolderView::hasSelection() const {
    QItemSelectionModel* selModel = selectionModel();
    return selModel ? selModel->hasSelection() : false;
}

QModelIndex FolderView::indexFromFolderPath(const Fm::FilePath& folderPath) const {
    if(!model_ || !folderPath.isValid()) {
        return QModelIndex();
    }
    QModelIndex index;
    int count = model_->rowCount();
    for(int row = 0; row < count; ++row) {
        index = model_->index(row, 0);
        auto info = model_->fileInfoFromIndex(index);
        if(info && info->isDir() && folderPath == info->path()) {
            return index;
        }
    }
    return QModelIndex();
}

bool FolderView::selectFiles(const Fm::FileInfoList& files, bool add) {
    if(!model_ || files.empty()) {
        return false;
    }
    QModelIndex index, firstIndex;
    int count = model_->rowCount();
    Fm::FileInfoList list = files;
    bool singleFile(files.size() == 1);
    QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::Select;
    if(mode == DetailedListMode) {
        flags |= QItemSelectionModel::Rows;
    }
    for(int row = 0; row < count; ++row) {
        if (list.empty()) {
            break;
        }
        index = model_->index(row, 0);
        auto info = model_->fileInfoFromIndex(index);
        for(auto it = list.cbegin(); it != list.cend(); ++it) {
            auto& item = *it;
            if(item == info) {
                if(model_->showHidden() || !info->isHidden()) {
                    if (!firstIndex.isValid()) {
                        firstIndex = index;
                        if(!add) {
                            selectionModel()->clear();
                        }
                    }
                    selectionModel()->select(index, flags);
                }
                list.erase(it);
                break;
            }
        }
    }
    if (firstIndex.isValid()) {
        view->scrollTo(firstIndex, QAbstractItemView::EnsureVisible);
        if (singleFile) { // give focus to the single file
            selectionModel()->setCurrentIndex(firstIndex, QItemSelectionModel::Current);
        }
        return true;
    }
    return false;
}

Fm::FileInfoList FolderView::selectedFiles() const {
    if(model_) {
        QModelIndexList selIndexes = mode == DetailedListMode ? selectedRows() : selectedIndexes();
        if(!selIndexes.isEmpty()) {
            Fm::FileInfoList files;
            QModelIndexList::const_iterator it;
            for(it = selIndexes.constBegin(); it != selIndexes.constEnd(); ++it) {
                auto file = model_->fileInfoFromIndex(*it);
                files.push_back(file);
            }
            return files;
        }
    }
    return Fm::FileInfoList();
}

void FolderView::selectAll() {
    view->selectAll();
}

void FolderView::invertSelection() {
    if(model_) {
        QItemSelectionModel* selModel = view->selectionModel();
        QItemSelectionModel::SelectionFlags flags;
        if(mode == DetailedListMode) {
            flags |= QItemSelectionModel::Rows;
        }
        // we don't use a "for" loop on rows because it would be slow
        const QItemSelection _all{model_->index(0, 0), model_->index(model_->rowCount() - 1, 0)};
        const QItemSelection _old{selModel->selection()};

        selModel->select(_all, flags | QItemSelectionModel::Select);
        selModel->select(_old, flags | QItemSelectionModel::Deselect);
    }
}

void FolderView::childDragEnterEvent(QDragEnterEvent* event) {
    //qDebug("drag enter");
    if(event->mimeData()->hasFormat(QStringLiteral("text/uri-list"))) {
        event->accept();
    }
    else {
        event->ignore();
    }
}

void FolderView::childDragLeaveEvent(QDragLeaveEvent* e) {
    //qDebug("drag leave");
    e->accept();
}

void FolderView::childDragMoveEvent(QDragMoveEvent* e) {
    // Since it isn't possible to drop on a file (see "FolderModel::dropMimeData()"),
    // we enable the drop indicator only when the cursor is on a folder.
    QModelIndex index = view->indexAt(e->position().toPoint());
    if(index.isValid() && index.model()) {
        QVariant data = index.model()->data(index, FolderModel::FileInfoRole);
        auto info = data.value<std::shared_ptr<const Fm::FileInfo>>();
        if(info && !info->isDir()) {
            view->setDropIndicatorShown(false);
            return;
        }
    }
    view->setDropIndicatorShown(true);
}

void FolderView::childDropEvent(QDropEvent* e) {
    // qDebug("drop");
    // Try to support XDS
    // NOTE: in theory, it's not possible to implement XDS with pure Qt.
    // We achieved this with some dirty XCB/XDND workarounds.
    // Please refer to XdndWorkaround::clientMessage() in xdndworkaround.cpp for details.
    if(QGuiApplication::platformName() == QStringLiteral("xcb") && e->mimeData()->hasFormat(QStringLiteral("XdndDirectSave0"))) {
        e->setDropAction(Qt::CopyAction);
        const QWidget* targetWidget = childView()->viewport();
        // these are dynamic QObject property set by our XDND workarounds in xdndworkaround.cpp.
        xcb_window_t dndSource = xcb_window_t(targetWidget->property("xdnd::lastDragSource").toUInt());
        //xcb_timestamp_t dropTimestamp = (xcb_timestamp_t)targetWidget->property("xdnd::lastDropTime").toUInt();
        // qDebug() << "XDS: source window" << dndSource << dropTimestamp;
        if(dndSource != 0) {
            xcb_atom_t XdndDirectSaveAtom = XdndWorkaround::internAtom("XdndDirectSave0", 15);
            xcb_atom_t textAtom = XdndWorkaround::internAtom("text/plain", 10);

            // 1. get the filename from XdndDirectSave property of the source window
            QByteArray basename = XdndWorkaround::windowProperty(dndSource, XdndDirectSaveAtom, textAtom, 1024);

            // 2. construct the fill URI for the file, and update the source window property.
            Fm::FilePath filePath;
            if(model_) {
                QModelIndex index = view->indexAt(e->position().toPoint());
                auto info = model_->fileInfoFromIndex(index);
                if(info && info->isDir()) {
                    filePath = info->path().child(basename.constData());
                }
            }
            if(!filePath.isValid()) {
                filePath = path().child(basename.constData());
            }
            QByteArray fileUri = filePath.uri().get();
            XdndWorkaround::setWindowProperty(dndSource,  XdndDirectSaveAtom, textAtom, (void*)fileUri.constData(), fileUri.length());

            // 3. send to XDS selection data request with type "XdndDirectSave" to the source window and
            //    receive result from the source window. (S: success, E: error, or F: failure)
            QByteArray result = e->mimeData()->data(QStringLiteral("XdndDirectSave0"));
            // NOTE: there seems to be some bugs in file-roller so it always replies with "E" even if the
            //       file extraction is finished successfully. Anyways, we ignore any error at the moment.
        }
        e->accept(); // yeah! we've done with XDS so stop Qt from further event propagation.
        return;
    }

    if(e->modifiers() == Qt::NoModifier) {
        // If no key modifiers are used, pop up a menu
        // to ask the user for the action he/she wants to perform.

        // NOTE: Under Wayland, serious problems will happen if the DND menu is shown
        // while the DND is in progress. Also, the menu needs a parent for correct positioning.

        Fm::FilePath destPath;
        std::shared_ptr<const Fm::FileInfo> info = nullptr;
        if(model_) {
            QModelIndex index = view->indexAt(e->position().toPoint());
            info = model_->fileInfoFromIndex(index);
        }
        if(info && info->isDir()) {
            destPath = info->path(); // drop on a subdirectory
        }
        else {
            info = folderInfo();
            destPath = path(); // drop on blank area of the folder
        }

        Fm::FilePathList srcPaths;
        // try to get paths from the original data
        if(e->mimeData()->hasFormat(QStringLiteral("libfm/files"))) {
            QByteArray _data = e->mimeData()->data(QStringLiteral("libfm/files"));
            srcPaths = pathListFromUriList(_data.data());
        }
        if(srcPaths.empty() && e->mimeData()->hasUrls()) {
            srcPaths = Fm::pathListFromQUrls(e->mimeData()->urls());
        }

        if(!srcPaths.empty()) {
            Qt::DropActions actions = Qt::IgnoreAction;
            if(info && info->isWritableDirectory() && info->isWritable()) {
                actions = e->possibleActions();
            }
            auto curPos = view->viewport()->mapToGlobal(e->position().toPoint());
            QTimer::singleShot(0, view, [this, curPos, actions, srcPaths, destPath] {
                Qt::DropAction action;
                // Wayland does not see the modifier if it is pressed after dragging.
                // Therefore, it should be checked now, when the DND is finished.
                switch(QApplication::keyboardModifiers()) {
                case Qt::ControlModifier:
                    action = Qt::CopyAction;
                    break;
                case Qt::ShiftModifier:
                    action = Qt::MoveAction;
                    break;
                case Qt::ControlModifier | Qt::ShiftModifier:
                    action = Qt::LinkAction;
                    break;
                default:
                    // a parent is needed under Wayland for correct positioning
                    action = DndActionMenu::askUser(actions, curPos, view);
                    break;
                }

                Q_EMIT dropIsDecided(action != Qt::IgnoreAction);

                switch(action) {
                case Qt::CopyAction:
                    FileOperation::copyFiles(srcPaths, destPath);
                    break;
                case Qt::MoveAction:
                    FileOperation::moveFiles(srcPaths, destPath);
                    break;
                case Qt::LinkAction:
                    FileOperation::symlinkFiles(srcPaths, destPath);
                    break;
                default:
                    break;
                }
            });
            e->accept(); // prevent further event propagation
            return;
        }
    }

    QTimer::singleShot(0, view, [this] {
        Q_EMIT dropIsDecided(true); // after finishing drop
    });
}

bool FolderView::eventFilter(QObject* watched, QEvent* event) {
    // NOTE: Instead of simply filtering the drag and drop events of the child view in
    // the event filter, we overrided each event handler virtual methods in
    // both QListView and QTreeView and added some childXXXEvent() callbacks.
    // We did this because of a design flaw of Qt.
    // All QAbstractScrollArea derived widgets, including QAbstractItemView
    // contains an internal child widget, which is called a viewport.
    // The events actually comes from the child viewport, not the parent view itself.
    // Qt redirects the events of viewport to the viewportEvent() method of
    // QAbstractScrollArea and let the parent widget handle the events.
    // Qt implemented this using a event filter installed on the child viewport widget.
    // That means, when we try to install an event filter on the viewport,
    // there is already a filter installed by Qt which will be called before ours.
    // So we can never intercept the event handling of QAbstractItemView by using a filter.
    // That's why we override respective virtual methods for different events.
    if(view && watched == view->viewport()) {
        switch(event->type()) {
        case QEvent::HoverMove:
        case QEvent::HoverEnter:
            // activate items on single click
            if(style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick)) {
                QHoverEvent* hoverEvent = static_cast<QHoverEvent*>(event);
                QModelIndex index = view->indexAt(hoverEvent->position().toPoint()); // find out the hovered item
                if(index.isValid()) { // change the cursor to a hand when hovering on an item
                    setCursor(Qt::PointingHandCursor);
                }
                else {
                    setCursor(Qt::ArrowCursor);
                }
                // turn on auto-selection for hovered item when single click mode is used.
                if(autoSelectionDelay_ > 0 && model_) {
                    if(!autoSelectionTimer_) {
                        autoSelectionTimer_ = new QTimer(this);
                        connect(autoSelectionTimer_, &QTimer::timeout, this, &FolderView::onAutoSelectionTimeout);
                        lastAutoSelectionIndex_ = QModelIndex();
                    }
                    autoSelectionTimer_->start(autoSelectionDelay_);
                }
            }
            break;
        case QEvent::HoverLeave:
            if(style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick)) {
                setCursor(Qt::ArrowCursor);
            }
            break;
        case QEvent::Wheel: {
            bool horizontalListView(false);

            // don't let the view scroll during an inline renaming
            FolderItemDelegate* delegate = nullptr;
            if(mode == DetailedListMode) {
                FolderViewTreeView* treeView = static_cast<FolderViewTreeView*>(view);
                delegate = static_cast<FolderItemDelegate*>(treeView->itemDelegateForColumn(FolderModel::ColumnFileName));
            }
            else {
                FolderViewListView* listView = static_cast<FolderViewListView*>(view);
                horizontalListView = (listView->flow() == QListView::TopToBottom);
                delegate = static_cast<FolderItemDelegate*>(listView->itemDelegateForColumn(FolderModel::ColumnFileName));
            }
            if(delegate && delegate->hasEditor()) {
                return true;
            }

            // Control scrolling with mouse wheel
            QWheelEvent* we = static_cast<QWheelEvent*>(event);
            QPoint angleDelta = we->angleDelta();
            bool horizontal(std::abs(angleDelta.x()) > std::abs(angleDelta.y()));
            if(event->spontaneous()
               && we->source() == Qt::MouseEventNotSynthesized
               // To have a simpler code, we control horizontal scrolling with mouse wheel
               // only in horizontal list views.
               && (horizontalListView || !horizontal)) {
                QScrollBar* sbar = horizontalListView ? view->horizontalScrollBar()
                                                      : view->verticalScrollBar();
                if(sbar && sbar->isVisible()) {
                    // first get the angle delta and customize it according to our needs
                    int delta = horizontal ? angleDelta.x() : angleDelta.y();
                    int origDelta = delta;
                    if(QApplication::wheelScrollLines() > 1) {
                        /* Scroll with minimum speed when
                            (1) The mode is compact (because columns can be wide), or
                            (2) Shift is pressed, or
                            (3) The angle delta is less than that of an ordinary mouse wheel, or
                            (4) The view has large icons. */
                        if(mode == CompactMode
                           || (we->modifiers() & Qt::ShiftModifier)
                           || std::abs(delta) < 120
                           || iconSize(mode).height() >= 96) {
                            if(std::abs(delta) >= QApplication::wheelScrollLines()) {
                                delta = delta / QApplication::wheelScrollLines();
                                // still slower scrolling with very large icons
                                if(iconSize(mode).height() >= 256 && std::abs(delta) >= 2) {
                                    delta /= 2;
                                }
                            }
                        }
                        else if(iconSize(mode).height() >= 64
                                && QApplication::wheelScrollLines() > 2
                                && std::abs(delta * 2) >= QApplication::wheelScrollLines()) {
                            // 2 rows per mouse turn for average icon sizes
                            delta = delta * 2 / QApplication::wheelScrollLines();
                        }
                    }

                    if((delta > 0 && sbar->value() == sbar->minimum()) || (delta < 0 && sbar->value() == sbar->maximum())) {
                        break; // the scrollbar can't move
                    }

                    if(!scrollPerPixel_) {
                        if(origDelta == delta) { // nothing has changed
                            break;
                        }
                        QWheelEvent e(we->position(),
                                      we->globalPosition(),
                                      we->pixelDelta(),
                                      QPoint (0, delta),
                                      we->buttons(),
                                      Qt::NoModifier,
                                      we->phase(),
                                      false,
                                      we->source());
                        QApplication::sendEvent(sbar, &e);
                        return true;
                    }
                    else {
                        // NOTE: Some touchpad devices may trigger wheel events with angle deltas
                        // less than "scrollAnimFrames", resulting in jumpy movements. Therefore,
                        // we wait until the total delta value is enough.
                        static int _delta = 0;
                        _delta += delta;
                        if(abs(_delta) < scrollAnimFrames) {
                            return true;
                        }

                        if(!smoothScrollTimer_) {
                            smoothScrollTimer_ = new QTimer();
                            connect(smoothScrollTimer_, &QTimer::timeout, this, &FolderView::scrollSmoothly);
                        }

                        // set the data for smooth scrolling
                        scrollData data;
                        data.delta = _delta;
                        data.leftFrames = scrollAnimFrames;
                        queuedScrollSteps_.append(data);
                        if(!smoothScrollTimer_->isActive()) {
                            smoothScrollTimer_->start(1000 / SCROLL_FRAMES_PER_SEC);
                        }
                        _delta = 0;
                        return true;
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void FolderView::scrollSmoothly() {
    QScrollBar* sbar = nullptr;
    if(mode == DetailedListMode) {
        sbar = view->verticalScrollBar();
    }
    else {
        FolderViewListView* listView = static_cast<FolderViewListView*>(view);
        if(listView->flow() == QListView::TopToBottom) {
            sbar = view->horizontalScrollBar();
        }
        else {
            sbar = view->verticalScrollBar();
        }
    }
    if(!sbar || !sbar->isVisible()) {
        queuedScrollSteps_.clear();
        smoothScrollTimer_->stop();
        return;
    }

    int totalDelta = 0;
    QList<scrollData>::iterator it = queuedScrollSteps_.begin();
    while(it != queuedScrollSteps_.end()) {
        int delta = std::round((qreal)it->delta / (qreal)scrollAnimFrames);
        int remainingDelta = it->delta - (scrollAnimFrames - it->leftFrames) * delta;
        if((delta >= 0 && remainingDelta < 0) || (delta < 0 && remainingDelta >= 0)) {
            remainingDelta = 0;
        }
        if(std::abs(delta) >= std::abs(remainingDelta)) {
            // this is the last frame or, due to rounding, there can be no more frame
            totalDelta += remainingDelta;
            it = queuedScrollSteps_.erase(it);
        }
        else {
            totalDelta += delta;
            -- it->leftFrames;
            ++it;
        }
    }
    if(totalDelta != 0) {
        QWheelEvent e(QPointF(),
                      QPointF(),
                      QPoint(),
                      QPoint (0, totalDelta),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::NoScrollPhase,
                      false);

        QApplication::sendEvent(sbar, &e);

        // update rubberband selection with smooth scrolling in icon view
        if ((mode == IconMode || mode == ThumbnailMode)
            && (QApplication::mouseButtons() & Qt::LeftButton)) {
            const QPoint globalPos = QCursor::pos();
            QPoint pos = view->viewport()->mapFromGlobal(globalPos);
            QMouseEvent ev(QEvent::MouseMove,
                           pos,
                           view->viewport()->mapTo(view->viewport()->topLevelWidget(), pos),
                           globalPos,
                           Qt::LeftButton,
                           Qt::LeftButton,
                           QApplication::keyboardModifiers());
            QApplication::sendEvent(view->viewport(), &ev);
        }
    }

    if(queuedScrollSteps_.empty()) {
        smoothScrollTimer_->stop();
    }
}

// this slot handles auto-selection of items.
void FolderView::onAutoSelectionTimeout() {
    if(QApplication::mouseButtons() != Qt::NoButton) {
        return;
    }

    // If the cursor moves immediately after a (context) menu is shown, "QEvent::HoverMove"
    // might be sent, which will result in calling this function. That is a Qt issue. As a
    // workaround, we do not proceed when there is an active popup widget.
    if(QApplication::activePopupWidget()) {
        return;
    }

    // don't do anything if the cursor is on selection corner icon
    if(mode != DetailedListMode) {
        FolderViewListView* listView = static_cast<FolderViewListView*>(view);
        if(listView->cursorOnSelectionCorner()) {
            return;
        }
    }

    QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos()); // convert to viewport coordinates
    QModelIndex index = view->indexAt(pos); // find out the hovered item
    if(!index.isValid()) {
        return;
    }

    Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
    QItemSelectionModel::SelectionFlags flags = (mode == DetailedListMode ? QItemSelectionModel::Rows : QItemSelectionModel::NoUpdate);
    QItemSelectionModel* selModel = view->selectionModel();

    if(mods & Qt::ControlModifier) { // Ctrl key is pressed
        if(selModel->isSelected(index) && index != lastAutoSelectionIndex_) {
            // unselect a previously selected item
            selModel->select(index, flags | QItemSelectionModel::Deselect);
            lastAutoSelectionIndex_ = QModelIndex();
        }
        else {
            // select an unselected item
            selModel->select(index, flags | QItemSelectionModel::Select);
            lastAutoSelectionIndex_ = index;
        }
        selModel->setCurrentIndex(index, QItemSelectionModel::NoUpdate); // move the cursor
    }
    else if(mods & Qt::ShiftModifier) { // Shift key is pressed
        // select all items between current index and the hovered index.
        QModelIndex current = selModel->currentIndex();
        if(selModel->hasSelection() && current.isValid()) {
            selModel->clear(); // clear old selection
            selModel->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
            int begin = current.row();
            int end = index.row();
            if(begin > end) {
                std::swap(begin, end);
            }
            for(int row = begin; row <= end; ++row) {
                QModelIndex sel = model_->index(row, 0);
                selModel->select(sel, flags | QItemSelectionModel::Select);
            }
        }
        else { // no items are selected, select the hovered item.
            if(index.isValid()) {
                selModel->select(index, flags | QItemSelectionModel::SelectCurrent);
                selModel->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            }
        }
        lastAutoSelectionIndex_ = index;
    }
    else if(mods == Qt::NoModifier) { // no modifier keys are pressed.
        if(index.isValid()) {
            // select the hovered item
            view->clearSelection();
            selModel->select(index, flags | QItemSelectionModel::SelectCurrent);
            selModel->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        }
        lastAutoSelectionIndex_ = index;
    }

    autoSelectionTimer_->deleteLater();
    autoSelectionTimer_ = nullptr;
}

void FolderView::onFileClicked(int type, const std::shared_ptr<const Fm::FileInfo> &fileInfo) {
    if(type == ActivatedClick) {
        if(fileLauncher_) {
            Fm::FileInfoList files;
            files.push_back(fileInfo);
            fileLauncher_->launchFiles(nullptr, std::move(files));
        }
    }
    else if(type == ContextMenuClick) {
        Fm::FilePath folderPath;
        bool isWritableDir(true);
        auto files = selectedFiles();
        if(!files.empty()) {
            auto& first = files.front();
            if(files.size() == 1 && first->isDir()) {
                folderPath = first->path();
                isWritableDir = first->isWritable();
            }
        }
        if(!folderPath.isValid()) {
            folderPath = path();
            if(auto info = folderInfo()) {
                isWritableDir = info->isWritable();
            }
        }
        QMenu* menu = nullptr;
        if(fileInfo) {
            // show context menu
            auto files = selectedFiles();
            if(!files.empty()) {
                Fm::FileMenu* fileMenu = new Fm::FileMenu(files, fileInfo, folderPath, isWritableDir, QString(), view);
                fileMenu->setFileLauncher(fileLauncher_);
                fileMenu->addTrustAction();
                prepareFileMenu(fileMenu);
                menu = fileMenu;
            }
        }
        if (!menu && folderInfo()) {
            Fm::FolderMenu* folderMenu = new Fm::FolderMenu(this, this);
            prepareFolderMenu(folderMenu);
            menu = folderMenu;
        }
        if(menu) {
            menu->exec(QCursor::pos());
            delete menu;
        }
    }
}

void FolderView::prepareFileMenu(FileMenu* /*menu*/) {
}

void FolderView::prepareFolderMenu(FolderMenu* /*menu*/) {
}
