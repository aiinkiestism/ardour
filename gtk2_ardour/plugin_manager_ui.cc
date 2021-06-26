/*
 * Copyright (C) 2021 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <cassert>
#include "pbd/unwind.h"

#include "ardour/types_convert.h"

#include "gtkmm2ext/gui_thread.h"

#include "plugin_manager_ui.h"
#include "plugin_scan_dialog.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

PluginManagerUI::PluginManagerUI ()
	: ArdourWindow (_("Plugin Manager"))
	, _btn_rescan_err (_("Re-scan Faulty"))
	, _btn_rescan_sel (_("Re-scan Selected"))
	, _btn_clear (_("Clear Stale Scan Log"))
	, _in_row_change (false)
{
	plugin_model = Gtk::ListStore::create (plugin_columns);

	Gtk::CellRendererToggle* cell_blacklist = Gtk::manage (new Gtk::CellRendererToggle ());
	Gtk::TreeViewColumn* column_blacklist   = Gtk::manage (new Gtk::TreeViewColumn (_("BL"), *cell_blacklist));

	cell_blacklist->property_activatable() = true;
	cell_blacklist->property_radio()       = false;
	column_blacklist->add_attribute (cell_blacklist->property_active (), plugin_columns.blacklisted);
	column_blacklist->add_attribute (cell_blacklist->property_activatable (), plugin_columns.can_blacklist);

	Gtk::CellRendererToggle* cell_fav = Gtk::manage (new Gtk::CellRendererToggle ());
	Gtk::TreeViewColumn* column_fav   = Gtk::manage (new Gtk::TreeViewColumn (_("Fav"), *cell_fav));

	cell_fav->property_activatable() = true;
	cell_fav->property_radio() = true;
	column_fav->add_attribute (cell_fav->property_active (), plugin_columns.favorite);
	column_fav->add_attribute (cell_fav->property_activatable (), plugin_columns.can_fav_hide);

	Gtk::CellRendererToggle* cell_hidden = Gtk::manage (new Gtk::CellRendererToggle ());
	Gtk::TreeViewColumn* column_hidden   = Gtk::manage (new Gtk::TreeViewColumn (_("Hide"), *cell_hidden));

	cell_hidden->property_activatable() = true;
	cell_hidden->property_radio() = true;
	column_hidden->add_attribute (cell_hidden->property_active (), plugin_columns.hidden);
	column_hidden->add_attribute (cell_hidden->property_activatable (), plugin_columns.can_fav_hide);

	plugin_display.append_column (_("Status"), plugin_columns.status);
	plugin_display.append_column (*column_blacklist);
	plugin_display.append_column (*column_fav);
	plugin_display.append_column (*column_hidden);
	plugin_display.append_column (_("Name"), plugin_columns.name);
	plugin_display.append_column (_("Creator"), plugin_columns.creator);
	plugin_display.append_column (_("Type"), plugin_columns.type);
	plugin_display.append_column (_("File/ID"), plugin_columns.path);

	plugin_display.set_model (plugin_model);
	plugin_display.set_headers_visible (true);
	plugin_display.set_headers_clickable (true);
	plugin_display.set_reorderable (false);
	plugin_display.set_rules_hint (true);
	plugin_display.set_enable_search(true);

	plugin_display.get_column (4)->set_resizable (true);
	plugin_display.get_column (5)->set_resizable (true);

#if 0
	if (UIConfiguration::instance().get_use_tooltips()) {
		recent_session_display.set_tooltip_column(1); // plugin_columns.tip
	}
#endif

	for (int i = 0; i < 8; ++i) {
		Gtk::TreeView::Column* column = plugin_display.get_column(i);
		if (column) {
			column->set_sort_column(i);
		}
	}

	plugin_model->set_sort_column (plugin_columns.name.index(), Gtk::SORT_ASCENDING);
	plugin_display.set_name("PluginSelectorDisplay");

	plugin_display.get_selection()->set_mode (Gtk::SELECTION_SINGLE);
	plugin_display.get_selection()->signal_changed().connect (sigc::mem_fun(*this, &PluginManagerUI::selection_changed));
	//plugin_display.signal_row_activated().connect_notify (sigc::mem_fun(*this, &PluginManagerUI::row_activated));

	_scroller.add (plugin_display);
	_scroller.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

	_log.set_editable (false);
	_log.set_wrap_mode (Gtk::WRAP_WORD);

	_log_scroller.set_shadow_type(Gtk::SHADOW_NONE);
	_log_scroller.set_border_width(0);
	_log_scroller.add (_log);
	_log_scroller.set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);

	_pane.add (_scroller);
	_pane.add (_log_scroller);
	_pane.set_divider (0, .85);

	Gtk::Label* lbl = new Gtk::Label (""); // spacer

	/* top level packing */
	_top.attach (*lbl,            0, 1, 0, 1, Gtk::SHRINK, Gtk::EXPAND | Gtk::FILL, 4, 0);
	_top.attach (_btn_clear,      0, 1, 1, 2, Gtk::FILL | Gtk::SHRINK, Gtk::SHRINK, 4, 4);
	_top.attach (_btn_rescan_sel, 0, 1, 2, 3, Gtk::FILL | Gtk::SHRINK, Gtk::SHRINK, 4, 4);
	_top.attach (_btn_rescan_err, 0, 1, 3, 4, Gtk::FILL | Gtk::SHRINK, Gtk::SHRINK, 4, 4);
	_top.attach (_pane,           1, 2, 0, 4, Gtk::EXPAND | Gtk::FILL, Gtk::EXPAND | Gtk::FILL, 4, 0);

	add (_top);

	_log.set_size_request (400, -1);
	set_size_request (-1, 600);

	/* connect to signals */

	PluginManager::instance ().PluginListChanged.connect (_manager_connections, invalidator (*this), boost::bind (&PluginManagerUI::refill, this), gui_context());
	PluginManager::instance ().PluginStatusChanged.connect (_manager_connections, invalidator (*this), boost::bind (&PluginManagerUI::plugin_status_changed, this, _1, _2, _3), gui_context());

	_btn_rescan_err.signal_clicked.connect (sigc::mem_fun (*this, &PluginManagerUI::rescan_faulty));
	_btn_rescan_sel.signal_clicked.connect (sigc::mem_fun (*this, &PluginManagerUI::rescan_selected));
	_btn_clear.signal_clicked.connect (sigc::mem_fun (*this, &PluginManagerUI::clear_log));

	cell_fav->signal_toggled().connect (sigc::mem_fun (*this, &PluginManagerUI::favorite_changed));
	cell_hidden->signal_toggled().connect (sigc::mem_fun (*this, &PluginManagerUI::hidden_changed));
	cell_blacklist->signal_toggled().connect (sigc::mem_fun (*this, &PluginManagerUI::blacklist_changed));

	/* populate */
	refill ();
}

PluginManagerUI::~PluginManagerUI ()
{
}

void
PluginManagerUI::on_show ()
{
	ArdourWindow::on_show ();
}

static std::string
status_text (PluginScanLogEntry const& psle, PluginManager::PluginStatusType status)
{
	if (!psle.recent ()) {
		return "Stale";
	}

	PluginScanLogEntry::PluginScanResult sr = psle.result ();
	if (sr == PluginScanLogEntry::OK || sr == PluginScanLogEntry::Blacklisted) {
		if (status == PluginManager::Concealed) {
			return _("Concealed");
		} else {
			return _("OK");
		}
	}

	if ((int)sr & PluginScanLogEntry::TimeOut) {
		return _("New");
	}
	if ((int)sr & PluginScanLogEntry::New) {
		return _("New");
	}
	if ((int)sr & PluginScanLogEntry::Updated) {
		return _("Updated");
	}
	if ((int)sr & PluginScanLogEntry::Error) {
		return _("Error");
	}
	if ((int)sr & PluginScanLogEntry::Incompatible) {
		return _("Incompatible");
	}
	assert (0);
	return "?";
}

static bool
is_blacklisted (PluginScanLogEntry const& psle)
{
	return (int) psle.result () & PluginScanLogEntry::Blacklisted;
}

static bool
can_blacklist (PluginScanLogEntry const& psle)
{
	if (psle.type () == LV2 || psle.type () == LADSPA) {
		return false;
	}
	return ((int) psle.result () & ~PluginScanLogEntry::Blacklisted) == PluginScanLogEntry::OK;
}

static std::string
plugin_type (const PluginType t)
{
	/* see also PluginManager::to_generic_vst */
	switch (t) {
		case Windows_VST:
		case LXVST:
		case MacVST:
			return "VST2.x";
		default:
			return enum_2_string (t);
	}
}

void
PluginManagerUI::refill ()
{
	Gtk::TreeIter iter = plugin_display.get_selection ()->get_selected ();
	boost::shared_ptr<PluginScanLogEntry> sel;
	if (iter) {
		sel = (*iter)[plugin_columns.psle];
	}

	std::vector<boost::shared_ptr<PluginScanLogEntry> > psl;
	PluginManager& mgr (PluginManager::instance ());
	mgr.scan_log (psl);

	plugin_display.set_model (Glib::RefPtr<Gtk::TreeStore>(0));

	int sort_col;
	Gtk::SortType sort_type;
	bool sorted = plugin_model->get_sort_column_id (sort_col, sort_type);
	plugin_model->set_sort_column (-2, Gtk::SORT_ASCENDING);

	bool rescan_err = false;
	bool have_stale = false;

	plugin_model->clear ();

	for (std::vector<boost::shared_ptr <PluginScanLogEntry> >::const_iterator i = psl.begin(); i != psl.end(); ++i) {
		PluginInfoList const& plugs = (*i)->nfo ();

		if (!(*i)->recent ()) {
			have_stale = true;
		} else if ((*i)->result () == PluginScanLogEntry::Blacklisted) {
			/* OK, but manually blacklisted */
		} else if ((*i)->result () != PluginScanLogEntry::OK) {
			if ((*i)->type () != LV2) {
				rescan_err = true;
			}
		}

		if (plugs.size () == 0) {
			Gtk::TreeModel::Row newrow = *(plugin_model->append());
			newrow[plugin_columns.path] = Glib::path_get_basename ((*i)->path ());
			newrow[plugin_columns.type] = plugin_type ((*i)->type ());
			newrow[plugin_columns.name] = "-";
			newrow[plugin_columns.creator] = "-";
			newrow[plugin_columns.status] = status_text (**i, PluginManager::Normal); // XXX
			newrow[plugin_columns.blacklisted] = is_blacklisted (**i);
			newrow[plugin_columns.psle] = *i;
			newrow[plugin_columns.plugin] = ARDOUR::PluginInfoPtr ();
			newrow[plugin_columns.favorite] = false;
			newrow[plugin_columns.hidden] = false;
			newrow[plugin_columns.can_blacklist] = can_blacklist (**i);
			newrow[plugin_columns.can_fav_hide] = false;
		} else {
			for (PluginInfoList::const_iterator j = plugs.begin(); j != plugs.end(); ++j) {
				PluginManager::PluginStatusType status = PluginManager::instance ().get_status (*j);
				Gtk::TreeModel::Row newrow = *(plugin_model->append());

				newrow[plugin_columns.favorite] = status == PluginManager::Favorite;
				newrow[plugin_columns.hidden] = status == PluginManager::Hidden;
				newrow[plugin_columns.path] = Glib::path_get_basename ((*i)->path ());
				newrow[plugin_columns.type] = plugin_type ((*i)->type ());
				newrow[plugin_columns.name] = (*j)->name;
				newrow[plugin_columns.creator] = (*j)->creator;
				newrow[plugin_columns.status] = status_text (**i, status);
				newrow[plugin_columns.blacklisted] = is_blacklisted (**i);
				newrow[plugin_columns.psle] = *i;
				newrow[plugin_columns.plugin] = *j;
				newrow[plugin_columns.can_blacklist] = can_blacklist (**i);
				newrow[plugin_columns.can_fav_hide] = status != PluginManager::Concealed;
			}
		}
	}
	plugin_display.set_model (plugin_model);
	if (sorted) {
		plugin_model->set_sort_column (sort_col, sort_type);
	}

	plugin_display.set_search_column (4); // Name

	if (sel) {
		Gtk::TreeModel::Children rows = plugin_model->children ();
		for (Gtk::TreeModel::Children::iterator i = rows.begin (); i != rows.end (); ++i) {
			boost::shared_ptr<PluginScanLogEntry> const& srow ((*i)[plugin_columns.psle]);
			if (*sel == *srow) {
				plugin_display.get_selection ()->select (*i);
				break;
			}
		}
	}

	_btn_clear.set_sensitive (have_stale);
	_btn_rescan_err.set_sensitive (rescan_err);
}

void
PluginManagerUI::selection_changed ()
{
	if (plugin_display.get_selection()->count_selected_rows() != 1) {
		_log.get_buffer()->set_text ("-");
		return;
	}

	Gtk::TreeIter iter = plugin_display.get_selection ()->get_selected ();
	boost::shared_ptr<PluginScanLogEntry> const& psle ((*iter)[plugin_columns.psle]);

	_log.get_buffer()->set_text (psle->log ());

	PluginScanLogEntry::PluginScanResult sr = psle->result ();
	if (sr == PluginScanLogEntry::OK || psle->type () == LV2) {
		_btn_rescan_sel.set_sensitive (false);
	} else {
		_btn_rescan_sel.set_sensitive (true);
	}
}

void
PluginManagerUI::blacklist_changed (std::string const& path)
{
	Gtk::TreeIter iter;
	if ((iter = plugin_model->get_iter (path))) {
		boost::shared_ptr<PluginScanLogEntry> const& psle ((*iter)[plugin_columns.psle]);
    if ((*iter)[plugin_columns.blacklisted]) {
			PluginScanDialog psd (false, true, this);
			PluginManager::instance ().rescan_plugin (psle->type (), psle->path ());
		} else {
			PluginManager::instance ().blacklist (psle->type (), psle->path ());
		}
	}
}

void
PluginManagerUI::rescan_faulty ()
{
	PluginScanDialog psd (false, true, this);
	PluginManager::instance ().rescan_faulty ();
}

void
PluginManagerUI::rescan_selected ()
{
	if (plugin_display.get_selection()->count_selected_rows() != 1) {
		return;
	}

	Gtk::TreeIter iter = plugin_display.get_selection ()->get_selected ();
	boost::shared_ptr<PluginScanLogEntry> const& psle ((*iter)[plugin_columns.psle]);

	PluginScanDialog psd (false, true, this);
	PluginManager::instance ().rescan_plugin (psle->type (), psle->path ());
}

void
PluginManagerUI::clear_log ()
{
	PluginManager::instance ().clear_stale_log ();
	refill ();
}

void
PluginManagerUI::plugin_status_changed (ARDOUR::PluginType t, std::string uid, ARDOUR::PluginManager::PluginStatusType stat)
{

	Gtk::TreeModel::Children rows = plugin_model->children ();
	for (Gtk::TreeModel::Children::iterator i = rows.begin (); i != rows.end (); ++i) {
		PluginInfoPtr pp = (*i)[plugin_columns.plugin];
		if (!pp || pp->type != t || pp->unique_id != uid) {
			continue;
		}

		(*i)[plugin_columns.favorite] = (stat == PluginManager::Favorite) ? true : false;
		(*i)[plugin_columns.hidden] = (stat == PluginManager::Hidden) ? true : false;

		break;
	}
}

void
PluginManagerUI::favorite_changed (const std::string& path)
{
  if (_in_row_change) {
    return;
  }

	PBD::Unwinder<bool> uw (_in_row_change, true);

	Gtk::TreeIter iter;
	if ((iter = plugin_model->get_iter (path))) {
    bool favorite = !(*iter)[plugin_columns.favorite];

    PluginManager::PluginStatusType status = (favorite ? PluginManager::Favorite : PluginManager::Normal);
		PluginInfoPtr pi = (*iter)[plugin_columns.plugin];

		ARDOUR::PluginManager& manager (PluginManager::instance ());
		manager.set_status (pi->type, pi->unique_id, status);
		manager.save_statuses ();
  }
}

void
PluginManagerUI::hidden_changed (const std::string& path)
{
  if (_in_row_change) {
    return;
  }

	PBD::Unwinder<bool> uw (_in_row_change, true);

	Gtk::TreeIter iter;
	if ((iter = plugin_model->get_iter (path))) {
		bool hidden = !(*iter)[plugin_columns.hidden];

		PluginManager::PluginStatusType status = (hidden ? PluginManager::Hidden : PluginManager::Normal);
		PluginInfoPtr pi = (*iter)[plugin_columns.plugin];

		ARDOUR::PluginManager& manager (PluginManager::instance ());
		manager.set_status (pi->type, pi->unique_id, status);
		manager.save_statuses ();
  }
}
