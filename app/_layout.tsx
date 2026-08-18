import { Stack } from 'expo-router';
import 'react-native-reanimated';

// Single-route app: app/index.tsx IS the whole UI (it renders its own tab
// bar). The starter template's (tabs) group used to anchor here, which made
// release builds open on the template's Welcome screen instead.
export default function RootLayout() {
  return <Stack screenOptions={{ headerShown: false }} />;
}
