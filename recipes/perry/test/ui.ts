import { App, VStack, Text, Button, State } from "perry/ui";
const count = State(0);
App({
    title: "perry conda test",
    width: 300,
    height: 200,
    body: VStack(12, [Text(`Count: ${count.value}`), Button("+1", () => count.set(count.value + 1))]),
});
