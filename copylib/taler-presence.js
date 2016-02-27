/* @licstart  The following is the entire license notice for the
  JavaScript code in this page.

  Copyright (C) 2016 GNUnet e.V.

  The JavaScript code in this page is free software: you can
  redistribute it and/or modify it under the terms of the GNU
  Lesser General Public License (GNU LGPL) as published by the Free Software
  Foundation, either version 2.1 of the License, or (at your option)
  any later version.  The code is distributed WITHOUT ANY WARRANTY;
  without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU LGPL for more details.

  As additional permission under GNU LGPL version 2.1 section 7, you
  may distribute non-source (e.g., minimized or compacted) forms of
  that code without the copy of the GNU LGPL normally required by
  section 4, provided you include this license notice and a URL
  through which recipients can access the Corresponding Source.

  @licend  The above is the entire license notice
  for the JavaScript code in this page.
*/

function handleInstall() {
  var show = document.getElementsByClassName("taler-installed-show");
  var hide = document.getElementsByClassName("taler-installed-hide");
  for (var i = 0; i < show.length; i++) {
    show[i].style.display = "";
  }
  for (var i = 0; i < hide.length; i++) {
    hide[i].style.display = "none";
  }
};

function handleUninstall() {
  var show = document.getElementsByClassName("taler-installed-show");
  var hide = document.getElementsByClassName("taler-installed-hide");
  for (var i = 0; i < show.length; i++) {
    show[i].style.display = "none";
  }
  for (var i = 0; i < hide.length; i++) {
    hide[i].style.display = "";
  }
};

function probeTaler() {
  var eve = new Event("taler-probe");
  console.log("probing taler");
  document.dispatchEvent(eve);
}

document.addEventListener("taler-wallet-present", handleInstall, false);
document.addEventListener("taler-unload", handleUninstall, false);
document.addEventListener("taler-load", handleInstall, false);

function initTaler() {
  handleUninstall();
  probeTaler();
}

window.addEventListener("load", initTaler, false);
