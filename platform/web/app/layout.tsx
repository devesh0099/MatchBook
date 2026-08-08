import type { Metadata } from 'next';
import './globals.css';
import { Nav } from '@/components/Nav';

export const metadata: Metadata = {
  title: 'Matching Engine Challenge',
  description: 'Implement order book matching in C++. Gated on correctness, ranked on latency.',
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>
        <div className="shell">
          <Nav />
          {children}
        </div>
      </body>
    </html>
  );
}
