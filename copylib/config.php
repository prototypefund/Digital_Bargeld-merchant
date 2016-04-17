<?php
/*
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

$REFUND_DELTA = 'P3M';
// set to false when done with local tests
$explicit_currency = "KUDOS";
$MERCHANT_CURRENCY = $explicit_currency;

$host = $_SERVER["HTTP_HOST"];
switch ($host) {
case "blog.demo.taler.net":
case "shop.demo.taler.net":
  $MERCHANT_CURRENCY = "KUDOS";
  break;
case "blog.test.taler.net":
case "shop.test.taler.net":
  $MERCHANT_CURRENCY = "PUDOS";
  break;
default:
  if(false == $explicit_currency){
    http_response_code (500);
    echo "<p>Bank configuration error: No currency for domain $host</p>\n";
    die();
  }
}
?>
