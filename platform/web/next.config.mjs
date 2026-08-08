/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  output: 'standalone',

  // In production Caddy puts the frontend and the API on ONE origin, so the
  // browser never makes a cross-origin request and there is no CORS to
  // configure. This rewrite reproduces that in dev, so the two environments do
  // not differ in a way that only shows up on event day.
  async rewrites() {
    const target = process.env.API_ORIGIN;
    if (!target) return [];
    return [{ source: '/api/:path*', destination: `${target}/:path*` }];
  },
};
export default nextConfig;
