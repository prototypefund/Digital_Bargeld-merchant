"use strict";


// JSX literals are compiled to calls to React.createElement calls.
let React = {
  createElement: function(tag, props, ...children) {
    let e = document.createElement(tag);
    for (let k in props) {
      e.setAttribute(k, props[k]);
    }
    for (let child of children) {
      if ("string" === typeof child || "number" == typeof child) {
        child = document.createTextNode(child);
      }
      e.appendChild(child);
    }
    return e;
  }
};

declare var h_contract: string;

document.addEventListener("DOMContentLoaded", function (e) {
  var eve = new CustomEvent('taler-execute-payment', {detail: {H_contract: h_contract}});
  document.dispatchEvent(eve);
});

function replace(el, r) {
  el.parentNode.replaceChild(r, el);
}

document.addEventListener("taler-payment-result", function (e: CustomEvent) {
  if (!e.detail.success) {
    alert("Payment failed\n" + JSON.stringify(e.detail));
  }
  console.log("finished payment");
  let msg = 
      <div>Payment successful.  View your <a href={e.detail.fulfillmentUrl}>product</a>.</div>;
  replace(document.getElementById("loading"), msg);
});
