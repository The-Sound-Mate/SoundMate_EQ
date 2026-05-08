import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Supabase Edge Function: generate-eq
// Proxy between SoundMate Client and Google Gemini API

Deno.serve(async (req: Request) => {
  // 1. CORS Handle
  if (req.method === 'OPTIONS') {
    return new Response('ok', { headers: { 'Access-Control-Allow-Origin': '*' } });
  }

  try {
    const { title, artist, genre, userPref, systemPref } = await req.json();
    const GEMINI_API_KEY = Deno.env.get('GEMINI_API_KEY');

    if (!GEMINI_API_KEY) {
      return new Response(JSON.stringify({ error: 'Server configuration error: AI Key missing' }), {
        status: 500,
        headers: { 'Content-Type': 'application/json' },
      });
    }

    const prompt = `Generate a 31-band equalizer setting for the song "${title}" by "${artist}". 
    Genre: ${genre || 'Unknown'}. 
    User Preference: ${userPref || 'None'}.
    System Goal: ${systemPref || 'Balanced and clear sound'}.
    Return ONLY a JSON array of 31 float numbers (gain in dB, -12 to 12). No text.`;

    const response = await fetch(
      `https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent?key=${GEMINI_API_KEY}`,
      {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          contents: [{ parts: [{ text: prompt }] }],
        }),
      }
    );

    const data = await response.json();
    
    // Simple parsing (Gemini result extraction)
    const textResult = data.candidates?.[0]?.content?.parts?.[0]?.text || "[]";
    const cleanedText = textResult.replace(/```json|```/g, "").trim();
    
    return new Response(cleanedText, {
      headers: { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' },
    });

  } catch (err) {
    return new Response(JSON.stringify({ error: err.message }), {
      status: 400,
      headers: { 'Content-Type': 'application/json' },
    });
  }
});
