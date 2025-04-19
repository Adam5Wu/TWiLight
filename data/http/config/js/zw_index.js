var DEVMODE = window.location.protocol === 'file:';
const SUB_PAGES = [
  { "title": "App Config", "anchor": "app-config", "url": "app.html" },
  { "title": "Network", "anchor": "wifi-config", "url": "wifi.html" },
  { "title": "Firmware", "anchor": "ota", "url": "ota.html" },
  { "title": "System", "anchor": "sys-mgmt", "url": "sysmgmt.html" }
];

function addTab(menu_tabs, entry) {
  menu_tabs.find("ul").append(`<li><a href="#${entry['anchor']}">${entry['title']}</a></li>`);
  var tab_frame = $(`<iframe scrolling="no" frameborder="0" loading="lazy" id="${entry['anchor']}"></iframe>`);
  tab_frame.on('load', function (evt) {
    $(this).contents().find("div#page-heading").addClass("hidden");
  });
  tab_frame.attr("src", entry['url']);
  menu_tabs.append(tab_frame);
}

$(function () {
  var menu_tabs = $("#config-tabs");
  for (var idx in SUB_PAGES) addTab(menu_tabs, SUB_PAGES[idx]);

  menu_tabs.tabs({
    activate: function (event, ui) {
      window.location.hash = ui.newTab.find('a').attr('href');
    }
  });

  var hash = window.location.hash;
  if (hash) {
    menu_tabs.tabs('option', 'active',
      menu_tabs.find('a[href="' + hash + '"]').parent().index());
  }
});