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

  include '../../copylib/util.php';
  include './blog_lib.php';

  $article = get($_GET['article']);
  if (null == $article){
    http_response_code(400);
    echo "Bad request (no article specified)";
    return;
  }

  session_start();
  $payments = &pull($_SESSION, "payments", array());
  $payments[$article] = array("ispayed" => true);
  $fulfillment_url = url_rel("essay_fulfillment.php");
  header("Location: $fulfillment_url");
  die();
?>
