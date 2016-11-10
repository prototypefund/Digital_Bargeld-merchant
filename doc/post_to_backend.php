function post_to_backend($backend_relative_url, $json)@{
  $url = "https://shop.com$backend_relative_url";

  $req = new http\Client\Request("POST",
                                 $url,
                                 array ("Content-Type" => "application/json"));

  $req->getBody()->append($json);

  // Execute the HTTP request
  $client = new http\Client;
  $client->enqueue($req)->send();
  return $client->getResponse();
@}
