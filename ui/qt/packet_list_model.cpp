/* packet_list_model.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <algorithm>

#include "packet_list_model.h"

#include "file.h"

#include <wsutil/nstime.h>
#include <epan/column.h>
#include <epan/prefs.h>

#include "ui/packet_list_utils.h"
#include "ui/recent.h"

#include "color.h"
#include "color_filters.h"
#include "frame_tvbuff.h"

#include "color_utils.h"
#include "wireshark_application.h"

#include <QColor>
#include <QFontMetrics>
#include <QModelIndex>
#include <QElapsedTimer>

PacketListModel::PacketListModel(QObject *parent, capture_file *cf) :
    QAbstractItemModel(parent),
    size_hint_enabled_(true),
    row_height_(-1),
    line_spacing_(0)
{
    setCaptureFile(cf);
    PacketListRecord::clearStringPool();
    connect(this, SIGNAL(itemHeightChanged(QModelIndex)),
            this, SLOT(emitItemHeightChanged(QModelIndex)),
            Qt::QueuedConnection);
}

void PacketListModel::setCaptureFile(capture_file *cf)
{
    cap_file_ = cf;
    resetColumns();
}

// Packet list records have no children (for now, at least).
QModelIndex PacketListModel::index(int row, int column, const QModelIndex &) const
{
    if (row >= visible_rows_.count() || row < 0 || !cap_file_ || column >= prefs.num_cols)
        return QModelIndex();

    PacketListRecord *record = visible_rows_[row];

    return createIndex(row, column, record);
}

// Everything is under the root.
QModelIndex PacketListModel::parent(const QModelIndex &) const
{
    return QModelIndex();
}

int PacketListModel::packetNumberToRow(int packet_num) const
{
    return number_to_row_.value(packet_num, -1);
}

guint PacketListModel::recreateVisibleRows()
{
    int pos = visible_rows_.count();
    PacketListRecord *record;

    beginResetModel();
    visible_rows_.clear();
    number_to_row_.clear();
    if (cap_file_) {
        PacketListRecord::resetColumns(&cap_file_->cinfo);
    }

    endResetModel();
    beginInsertRows(QModelIndex(), pos, pos);
    foreach (record, physical_rows_) {
        if (record->frameData()->flags.passed_dfilter || record->frameData()->flags.ref_time) {
            visible_rows_ << record;
            number_to_row_[record->frameData()->num] = visible_rows_.count() - 1;
        }
    }
    endInsertRows();
    return visible_rows_.count();
}

void PacketListModel::clear() {
    beginResetModel();
    qDeleteAll(physical_rows_);
    physical_rows_.clear();
    visible_rows_.clear();
    number_to_row_.clear();
    PacketListRecord::clearStringPool();
    endResetModel();
}

void PacketListModel::resetColumns()
{
    if (cap_file_) {
        PacketListRecord::resetColumns(&cap_file_->cinfo);
    }
    dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
}

void PacketListModel::resetColorized()
{
    foreach (PacketListRecord *record, physical_rows_) {
        record->resetColorized();
    }
    dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

void PacketListModel::toggleFrameMark(const QModelIndex &fm_index)
{
    if (!cap_file_ || !fm_index.isValid()) return;

    PacketListRecord *record = static_cast<PacketListRecord*>(fm_index.internalPointer());
    if (!record) return;

    frame_data *fdata = record->frameData();
    if (!fdata) return;

    if (fdata->flags.marked)
        cf_unmark_frame(cap_file_, fdata);
    else
        cf_mark_frame(cap_file_, fdata);

    dataChanged(fm_index, fm_index);
}

void PacketListModel::setDisplayedFrameMark(gboolean set)
{
    foreach (PacketListRecord *record, visible_rows_) {
        if (set) {
            cf_mark_frame(cap_file_, record->frameData());
        } else {
            cf_unmark_frame(cap_file_, record->frameData());
        }
    }
    dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

void PacketListModel::toggleFrameIgnore(const QModelIndex &i_index)
{
    if (!cap_file_ || !i_index.isValid()) return;

    PacketListRecord *record = static_cast<PacketListRecord*>(i_index.internalPointer());
    if (!record) return;

    frame_data *fdata = record->frameData();
    if (!fdata) return;

    if (fdata->flags.ignored)
        cf_unignore_frame(cap_file_, fdata);
    else
        cf_ignore_frame(cap_file_, fdata);
}

void PacketListModel::setDisplayedFrameIgnore(gboolean set)
{
    foreach (PacketListRecord *record, visible_rows_) {
        if (set) {
            cf_ignore_frame(cap_file_, record->frameData());
        } else {
            cf_unignore_frame(cap_file_, record->frameData());
        }
    }
    dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

void PacketListModel::toggleFrameRefTime(const QModelIndex &rt_index)
{
    if (!cap_file_ || !rt_index.isValid()) return;

    PacketListRecord *record = static_cast<PacketListRecord*>(rt_index.internalPointer());
    if (!record) return;

    frame_data *fdata = record->frameData();
    if (!fdata) return;

    if (fdata->flags.ref_time) {
        fdata->flags.ref_time=0;
        cap_file_->ref_time_count--;
    } else {
        fdata->flags.ref_time=1;
        cap_file_->ref_time_count++;
    }
    cf_reftime_packets(cap_file_);
    if (!fdata->flags.ref_time && !fdata->flags.passed_dfilter) {
        cap_file_->displayed_count--;
    }
    record->resetColumns(&cap_file_->cinfo);
    dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

void PacketListModel::unsetAllFrameRefTime()
{
    if (!cap_file_) return;

    /* XXX: we might need a progressbar here */

    foreach (PacketListRecord *record, physical_rows_) {
        frame_data *fdata = record->frameData();
        if (fdata->flags.ref_time) {
            fdata->flags.ref_time = 0;
        }
    }
    cap_file_->ref_time_count = 0;
    cf_reftime_packets(cap_file_);
    PacketListRecord::resetColumns(&cap_file_->cinfo);
    dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

void PacketListModel::setMonospaceFont(const QFont &mono_font, int row_height)
{
    QFontMetrics fm(mono_font_);
    mono_font_ = mono_font;
    row_height_ = row_height;
    line_spacing_ = fm.lineSpacing();
}

// The Qt MVC documentation suggests using QSortFilterProxyModel for sorting
// and filtering. That seems like overkill but it might be something we want
// to do in the future.

int PacketListModel::sort_column_;
int PacketListModel::text_sort_column_;
Qt::SortOrder PacketListModel::sort_order_;
capture_file *PacketListModel::sort_cap_file_;

QElapsedTimer busy_timer_;
const int busy_timeout_ = 65; // ms, approximately 15 fps
void PacketListModel::sort(int column, Qt::SortOrder order)
{
    // packet_list_store.c:packet_list_dissect_and_cache_all
    if (!cap_file_ || visible_rows_.count() < 1) return;
    if (column < 0) return;

    sort_column_ = column;
    text_sort_column_ = PacketListRecord::textColumn(column);
    sort_order_ = order;
    sort_cap_file_ = cap_file_;

    gboolean stop_flag = FALSE;
    QString col_title = get_column_title(column);

    busy_timer_.start();
    emit pushProgressStatus(tr("Dissecting"), true, true, &stop_flag);
    int row_num = 0;
    foreach (PacketListRecord *row, physical_rows_) {
        row->columnString(sort_cap_file_, column);
        row_num++;
        if (busy_timer_.elapsed() > busy_timeout_) {
            if (stop_flag) {
                emit popProgressStatus();
                return;
            }
            emit updateProgressStatus(row_num * 100 / physical_rows_.count());
            // What's the least amount of processing that we can do which will draw
            // the progress indicator?
            wsApp->processEvents(QEventLoop::AllEvents, 1);
            busy_timer_.restart();
        }
    }
    emit popProgressStatus();

    // XXX Use updateProgress instead. We'd have to switch from std::sort to
    // something we can interrupt.
    if (!col_title.isEmpty()) {
        QString busy_msg = tr("Sorting \"%1\"").arg(col_title);
        emit pushBusyStatus(busy_msg);
    }

    busy_timer_.restart();
    std::sort(physical_rows_.begin(), physical_rows_.end(), recordLessThan);

    beginResetModel();
    visible_rows_.clear();
    number_to_row_.clear();
    foreach (PacketListRecord *record, physical_rows_) {
        if (record->frameData()->flags.passed_dfilter || record->frameData()->flags.ref_time) {
            visible_rows_ << record;
            number_to_row_[record->frameData()->num] = visible_rows_.count() - 1;
        }
    }
    endResetModel();

    if (!col_title.isEmpty()) {
        emit popBusyStatus();
    }

    if (cap_file_->current_frame) {
        emit goToPacket(cap_file_->current_frame->num);
    }
}

bool PacketListModel::recordLessThan(PacketListRecord *r1, PacketListRecord *r2)
{
    int cmp_val = 0;

    // Wherein we try to cram the logic of packet_list_compare_records,
    // _packet_list_compare_records, and packet_list_compare_custom from
    // gtk/packet_list_store.c into one function

    if (busy_timer_.elapsed() > busy_timeout_) {
        // What's the least amount of processing that we can do which will draw
        // the busy indicator?
        wsApp->processEvents(QEventLoop::ExcludeUserInputEvents | QEventLoop::ExcludeSocketNotifiers, 1);
        busy_timer_.restart();
    }
    if (sort_column_ < 0) {
        // No column.
        cmp_val = frame_data_compare(sort_cap_file_->epan, r1->frameData(), r2->frameData(), COL_NUMBER);
    } else if (text_sort_column_ < 0) {
        // Column comes directly from frame data
        cmp_val = frame_data_compare(sort_cap_file_->epan, r1->frameData(), r2->frameData(), sort_cap_file_->cinfo.columns[sort_column_].col_fmt);
    } else  {
        if (r1->columnString(sort_cap_file_, sort_column_).constData() == r2->columnString(sort_cap_file_, sort_column_).constData()) {
            cmp_val = 0;
        } else if (sort_cap_file_->cinfo.columns[sort_column_].col_fmt == COL_CUSTOM) {
            header_field_info *hfi;

            // Column comes from custom data
            hfi = proto_registrar_get_byname(sort_cap_file_->cinfo.columns[sort_column_].col_custom_field);

            if (hfi == NULL) {
                cmp_val = frame_data_compare(sort_cap_file_->epan, r1->frameData(), r2->frameData(), COL_NUMBER);
            } else if ((hfi->strings == NULL) &&
                       (((IS_FT_INT(hfi->type) || IS_FT_UINT(hfi->type)) &&
                         ((hfi->display == BASE_DEC) || (hfi->display == BASE_DEC_HEX) ||
                          (hfi->display == BASE_OCT))) ||
                        (hfi->type == FT_DOUBLE) || (hfi->type == FT_FLOAT) ||
                        (hfi->type == FT_BOOLEAN) || (hfi->type == FT_FRAMENUM) ||
                        (hfi->type == FT_RELATIVE_TIME)))
            {
                // Attempt to convert to numbers.
                // XXX This is slow. Can we avoid doing this?
                bool ok_r1, ok_r2;
                double num_r1 = r1->columnString(sort_cap_file_, sort_column_).toDouble(&ok_r1);
                double num_r2 = r2->columnString(sort_cap_file_, sort_column_).toDouble(&ok_r2);

                if (!ok_r1 && !ok_r2) {
                    cmp_val = 0;
                } else if (!ok_r1 || num_r1 < num_r2) {
                    cmp_val = -1;
                } else if (!ok_r2 || num_r1 > num_r2) {
                    cmp_val = 1;
                }
            } else {
                cmp_val = strcmp(r1->columnString(sort_cap_file_, sort_column_).constData(), r2->columnString(sort_cap_file_, sort_column_).constData());
            }
        } else {
            cmp_val = strcmp(r1->columnString(sort_cap_file_, sort_column_).constData(), r2->columnString(sort_cap_file_, sort_column_).constData());
        }

        if (cmp_val == 0) {
            // All else being equal, compare column numbers.
            cmp_val = frame_data_compare(sort_cap_file_->epan, r1->frameData(), r2->frameData(), COL_NUMBER);
        }
    }

    if (sort_order_ == Qt::AscendingOrder) {
        return cmp_val < 0;
    } else {
        return cmp_val > 0;
    }
}

void PacketListModel::emitItemHeightChanged(const QModelIndex &ih_index)
{
    emit dataChanged(ih_index, ih_index);
}

int PacketListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() >= prefs.num_cols)
        return 0;

    return visible_rows_.count();
}

int PacketListModel::columnCount(const QModelIndex &) const
{
    return prefs.num_cols;
}

QVariant PacketListModel::data(const QModelIndex &d_index, int role) const
{
    if (!d_index.isValid())
        return QVariant();

    PacketListRecord *record = static_cast<PacketListRecord*>(d_index.internalPointer());
    if (!record)
        return QVariant();
    const frame_data *fdata = record->frameData();
    if (!fdata)
        return QVariant();

    switch (role) {
    case Qt::FontRole:
        return mono_font_;
    case Qt::TextAlignmentRole:
        switch(recent_get_column_xalign(d_index.column())) {
        case COLUMN_XALIGN_RIGHT:
            return Qt::AlignRight;
            break;
        case COLUMN_XALIGN_CENTER:
            return Qt::AlignCenter;
            break;
        case COLUMN_XALIGN_LEFT:
            return Qt::AlignLeft;
            break;
        case COLUMN_XALIGN_DEFAULT:
        default:
            if (right_justify_column(d_index.column(), cap_file_)) {
                return Qt::AlignRight;
            }
            break;
        }
        return Qt::AlignLeft;

    case Qt::BackgroundRole:
        const color_t *color;
        if (fdata->flags.ignored) {
            color = &prefs.gui_ignored_bg;
        } else if (fdata->flags.marked) {
            color = &prefs.gui_marked_bg;
        } else if (fdata->color_filter && recent.packet_list_colorize) {
            const color_filter_t *color_filter = (const color_filter_t *) fdata->color_filter;
            color = &color_filter->bg_color;
        } else {
            return QVariant();
        }
        return ColorUtils::fromColorT(color);
    case Qt::ForegroundRole:
        if (fdata->flags.ignored) {
            color = &prefs.gui_ignored_fg;
        } else if (fdata->flags.marked) {
            color = &prefs.gui_marked_fg;
        } else if (fdata->color_filter && recent.packet_list_colorize) {
            const color_filter_t *color_filter = (const color_filter_t *) fdata->color_filter;
            color = &color_filter->fg_color;
        } else {
            return QVariant();
        }
        return ColorUtils::fromColorT(color);
    case Qt::DisplayRole:
    {
        int column = d_index.column();
        QByteArray column_string = record->columnString(cap_file_, column, true);
        // We don't know an item's sizeHint until we fetch its text here.
        // Assume each line count is 1. If the line count changes, emit
        // itemHeightChanged which triggers another redraw (including a
        // fetch of SizeHintRole and DisplayRole) in the next event loop.
        if (column == 0 && record->lineCountChanged())
            emit itemHeightChanged(d_index);
        return column_string;
    }
    case Qt::SizeHintRole:
    {
        if (size_hint_enabled_) {
            // We assume that inter-line spacing is 0.
            QSize size = QSize(-1, row_height_ + ((record->lineCount() - 1) * line_spacing_));
            return size;
        } else {
            // Used by PacketList::sizeHintForColumn
            return QVariant();
        }
    }
    default:
        return QVariant();
    }

    return QVariant();
}

QVariant PacketListModel::headerData(int section, Qt::Orientation orientation,
                               int role) const
{
    if (!cap_file_) return QVariant();

    if (orientation == Qt::Horizontal && section < prefs.num_cols) {
        switch (role) {
        case Qt::DisplayRole:
            return get_column_title(section);
        default:
            break;
        }
    }

    return QVariant();
}

gint PacketListModel::appendPacket(frame_data *fdata)
{
    PacketListRecord *record = new PacketListRecord(fdata);
    gint pos = visible_rows_.count();

    physical_rows_ << record;

    if (fdata->flags.passed_dfilter || fdata->flags.ref_time) {
        beginInsertRows(QModelIndex(), pos, pos);
        visible_rows_ << record;
        number_to_row_[fdata->num] = visible_rows_.count() - 1;
        endInsertRows();
    } else {
        pos = -1;
    }
    return pos;
}

frame_data *PacketListModel::getRowFdata(int row) {
    if (row < 0 || row >= visible_rows_.count())
        return NULL;
    PacketListRecord *record = visible_rows_[row];
    if (!record)
        return NULL;
    return record->frameData();
}

void PacketListModel::ensureRowColorized(int row)
{
    if (row < 0 || row >= visible_rows_.count())
        return;
    PacketListRecord *record = visible_rows_[row];
    if (!record)
        return;
    if (!record->colorized()) {
        record->columnString(cap_file_, 1, true);
    }
}

int PacketListModel::visibleIndexOf(frame_data *fdata) const
{
    int row = 0;
    foreach (PacketListRecord *record, visible_rows_) {
        if (record->frameData() == fdata) {
            return row;
        }
        row++;
    }

    return -1;
}

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
