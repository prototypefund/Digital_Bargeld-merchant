<?php
/*
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

  include("../../copylib/merchants.php");
  include("../../copylib/util.php");
  include("./blog_lib.php");
  
  log_string("paying");

  $article = get($_GET["article"]);
  if (empty($article)){
    http_response_code(400);
    echo json_encode(array(
      "error" => "missing parameter",
      "parameter" => "article"
    ));
    return;
  }
  $deposit_permission = file_get_contents('php://input');
  // FIXME check here if the deposit permission is associated
  session_start();
  if (!isset($_SESSION["payments"])) {
    $json = json_encode(
      array(
        "error" => "no payments ongoing",
        "status" => 500,
        "detail" => "the shop has no state for any article"
      )
    );
    echo $json;
    die();
  }
  $payments = &pull($_SESSION, "payments", array());
  $dec_dep_perm = json_decode($deposit_permission, true);
  if ($dec_dep_perm['H_contract'] != $payments[$article]['hc']){
    $json = json_encode(
      array(
        "error" => "ill behaved wallet",
        "status" => 400,
        "detail" => "article payed differs from article to be shown"
      )
    );
    echo $json;
    die();
  }
  // FIXME put some control below

  // with the article that's going to be payed
  $resp = give_to_backend("backend/pay",
                          $deposit_permission);
  $status_code = $resp->getResponseCode();
  http_response_code ($status_code);
  if ($status_code != 200)
  {
    $json = json_encode(
      array(
        "error" => "backend error",
        "status" => $status_code,
        "detail" => $resp->body->toString()));
    echo $json;
    die();
  }
  $payments[$article]['ispayed'] = true;
?>
