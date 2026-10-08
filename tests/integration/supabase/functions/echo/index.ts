Deno.serve(async (req) => {
  const body = await req.json().catch(() => ({}));
  return new Response(JSON.stringify({ echo: body }), {
    headers: { "Content-Type": "application/json" },
  });
});
