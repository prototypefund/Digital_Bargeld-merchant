<?php
/*
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
  include '../../copylib/util.php';
  include '../../copylib/merchants.php';
  include '../../copylib/config.php';
  include './blog_lib.php';
  
  $article = get($_GET['article']);
  if (null == $article){
    http_response_code(400);
    echo message_from_missing_param("article", "/");
    return;
  }
  session_start();

  $payments = &pull($_SESSION, 'payments', array());
  $my_payment = &pull($payments, $article, false);

  $pay_url = url_rel("essay_pay.php");
  $offering_url = url_rel("essay_fulfillment.php", true);
  $offering_url .= "?article=$article";

  // In PHP false == null
  if (null == get($payments[$article]['ispayed']) || false == $my_payment){
    $tid = get($_GET['tid']);
    $timestamp = get($_GET['timestamp']);
    // 1st time
    if (null == $tid || null == $timestamp){
      $js_code = "get_contract(\"$article\");";
      $cc_page = template("./essay_cc-form.html", array('article' => $article, 'jscode' => $js_code));
      echo $cc_page;
      log_string("cnt blog");
      return;
    }
    log_string("restoring blog");
    //log_string("state: " . print_r($_SESSION, true));
    // restore contract
    $now = new DateTime();
    $now->setTimestamp(intval($timestamp));

    $contract_rec = generate_contract(array(
      "amount_value" => 0,
      "amount_fraction" => 50000,
      "currency" => $MERCHANT_CURRENCY,
      "refund_delta" => $REFUND_DELTA,
      "merchant_name" => "Free Software Foundation (demo)",
      "transaction_id" => intval($tid),
      "description" => trim(get_title($article)),
      "product_id" => $article,
      "correlation_id" => $article,
      "taxes" => array(),
      "now" => $now,
      "fulfillment_url" => get_full_uri())
    );
    $resp = give_to_backend("backend/contract",
                            $contract_rec);
    if ($resp->getResponseCode() != 200){
      echo json_encode(array(
        'error' => "internal error",
        'hint' => "non hashable contract",
        'detail' => $resp->body->toString()
      ), JSON_PRETTY_PRINT);
      die();
    }
    $hc = json_decode($resp->body->toString(), true)['H_contract'];
    $my_payment['hc'] = $hc;
    log_string("sending payment event");
    $js_code = "executePayment(\"$hc\", \"$pay_url\", \"$offering_url\");";
    $cc_page = template("./essay_cc-form.html", array('article' => $article, 'jscode' => $js_code));
    echo $cc_page;
    return;
    }
  // control here == article payed
  log_string("arti blog");
  $article = get_article($article);
  echo $article;
?>
