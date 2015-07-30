/*

  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>


*/

/* Set up a listener to be called whenever a Wallet gets installed
so that the user is led towards the demo's steps progressively
*/

function MERCHfirstStep(){

 // NOTE: NO 'let' declarations liked by FF here.
 var resDiv = document.createElement("div");
 var resTitle = document.createElement("h3");
 var resPar = document.createElement("p");

 resTitle.innerHTML = "How To Create A Reserve";
 resPar.innerHTML = "Click on 'Create Reserve' on the Wallet's menu, and fill in the followong form"

 resTitle.setAttribute('class', 'preamble');
 resDiv.appendChild(resTitle);
 resDiv.appendChild(resPar);
 var root = document.getElementById('root');
 root.appendChild(resDiv);

 // get the form
 var getform = new XMLHttpRequest();

 getform.onload = function (){
 var parser = new DOMParser();
 var formDom = parser.parseFromString(getform.responseText, "text/html");
 var form = formDom.getElementById('reserve-form');
 
 resDiv.appendChild(form);
 
 };


 getform.open("GET", "create-reserve-form.html", true);
 getform.send();
 
}

document.body.addEventListener("taler-wallet-installed", MERCHfirstStep, false, false);


/*
notify the extension about the submission. That way it will be possible to retrieve
the mint's URL and/or other info. from the filled form. Actually, the extension will
accomplish the POST too.

*/
function MERCHtrigSubmission(){

// set 'action' attribute to mint's url
// var mint = document.getElementById("mint-url");

// var form = document.getElementById("reserve-form");
// form.setAttribute("action", "http://" + mint.value + "/admin/incoming/add");

var subEvent = new Event("submit-reserve");
document.body.dispatchEvent(subEvent);

// always return false so that the post is actually done by the extension
return false;

}
